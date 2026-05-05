#include "ShaderPatch.h"
#include "DustLog.h"
#include "SurveyRecorder.h"
#include "D3D11Hook.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <string>
#include <cstring>
#include <cstdio>

namespace ShaderPatch
{

PFN_D3DCompileHook oD3DCompile = nullptr;

// Patch vanilla deferred.hlsl source to add AO support and improved shadow filtering.
// Returns the modified source, or the original if patterns weren't found.
static std::string PatchDeferredShader(const std::string& src)
{
    std::string result = src;

    // === AO Patches ===

    // Injection 1: Add aoMap + aoParams sampler declarations.
    // Anchor: "uniform float4 ambientParams," exists in all variants.
    const char* anchor1 = "uniform float4 ambientParams,";
    size_t pos1 = result.find(anchor1);
    if (pos1 == std::string::npos)
    {
        Log("ShaderPatch: anchor 'ambientParams' not found, skipping");
        return src;
    }

    std::string inject1 =
        "uniform sampler aoMap : register(s8),\n"
        "\tuniform sampler aoParams : register(s9),\n\n\t";
    result.insert(pos1, inject1);

    // Injection 2: Add AO application code.
    // Anchor: "LightingData ld = (LightingData)0.0f;" — right after env light calculation.
    const char* anchor2 = "LightingData ld = (LightingData)0.0f;";
    size_t pos2 = result.find(anchor2);
    if (pos2 == std::string::npos)
    {
        Log("ShaderPatch: anchor 'LightingData ld' not found, skipping");
        return src;
    }

    std::string inject2 =
        "// [Dust] Ambient occlusion\n"
        "\tfloat ao = tex2D(aoMap, texCoord).r;\n"
        "\tfloat directAO = tex2D(aoParams, texCoord).r;\n"
        "\tenvLight.diffuse *= ao;\n"
        "\tenvLight.specular *= ao;\n"
        "\tfloat directFade = lerp(1.0, ao, directAO);\n"
        "\tsunLight.diffuse *= directFade;\n"
        "\tsunLight.specular *= directFade;\n\n\t";
    result.insert(pos2, inject2);

    // === Shadow Patches ===
    // Replace vanilla RTWShadow (3x3 PCF with 0.0001 texel size — essentially a single sample)
    // with improved filtering: 12-sample Poisson disk, per-pixel rotation, PCSS penumbra.
    // Parameters come from a constant buffer (b2) bound by the Shadows effect plugin.

    // Injection 3: Add cbuffer declaration + DustRTWShadow function.
    // Insert before main_vs so it's defined after includes (GetOffsetLocationS, ShadowMap)
    // but before use in main_fs.
    // If the "Cliff Face Shadow Fix" workshop mod is present, its steep bias is already
    // baked into the shadow_bias parameter passed to RTWShadow — skip our own to avoid doubling.
    bool workshopSteepBias = (result.find("steepBias") != std::string::npos);
    if (workshopSteepBias)
        Log("ShaderPatch: detected workshop steep bias mod, skipping internal steep bias");

    const char* anchor3 = "void main_vs (";
    size_t pos3 = result.find(anchor3);
    if (pos3 != std::string::npos)
    {
        std::string steepBlock = workshopSteepBias ?
            "\tfloat ny = abs(normal.y);\n"
            "\tfloat steep = saturate((0.42 - ny) * 4.25);\n"
            "\tfloat farGate = saturate((dist - shadowRange * 0.10) * 0.0035);\n"
            "\tb -= (steep * steep) * farGate * 0.0032;\n"
            "\n"
            :
            "";

        std::string inject3 =
            // Use b7 not b2: CSM's auto-allocated $Globals cbuffer can land
            // on b2 due to its larger uniform array footprint (csmParams,
            // csmScale, csmTrans, csmUvBounds = 4 * SHADOW_MAP_COUNT vec4s).
            // Our plugin's PSSetConstantBuffers(7, ...) then doesn't clobber
            // game data. RTW happened to work at b2 because its uniform set
            // is much smaller and stays in b0.
            "// [Dust] Shadow filtering parameters (bound by Shadows plugin at b7)\n"
            "cbuffer DustShadowParams : register(b7) {\n"
            "\tfloat dustShadowEnabled;\n"
            "\tfloat dustFilterRadius;\n"
            "\tfloat dustLightSize;\n"
            "\tfloat dustPCSSEnabled;\n"
            "\tfloat dustBiasScale;\n"
            "\tfloat dustCliffFixEnabled;\n"
            "\tfloat dustCliffFixDistance;\n"
            "\tfloat dustCsmFilterScale;\n"
            "};\n\n"
            // The warp map is 513x2 R32_FLOAT, sampled by vanilla GetOffsetLocationS
            // with tex2Dlod. The warp sampler is point-filtered, so adjacent screen
            // pixels can fall into different warp-map texels and snap to discretely
            // different shadow-map lookups — visible as "squares" whose screen size
            // grows with the warp gradient. Manually bilerp the warp value to remove
            // that quantization (4 taps total, vs vanilla's 2; warp map is tiny, all
            // taps stay in cache).
            "// [Dust] Bilinear warp lookup (replacement for point-sampled GetOffsetLocationS)\n"
            "float DustWarp1D(sampler2D wMap, float u, float row) {\n"
            "\tconst float kWarpW = 513.0;\n"
            "\tfloat p = u * kWarpW - 0.5;\n"
            "\tfloat pf = clamp(floor(p), 0.0, kWarpW - 2.0);\n"
            "\tfloat t = saturate(p - pf);\n"
            "\tfloat u0 = (pf + 0.5) / kWarpW;\n"
            "\tfloat u1 = (pf + 1.5) / kWarpW;\n"
            "\tfloat v0 = tex2Dlod(wMap, float4(u0, row, 0, 0)).x;\n"
            "\tfloat v1 = tex2Dlod(wMap, float4(u1, row, 0, 0)).x;\n"
            "\treturn lerp(v0, v1, t);\n"
            "}\n"
            "float2 DustGetOffsetLocationS(sampler2D wMap, float2 ts) {\n"
            "\tts.x += DustWarp1D(wMap, ts.x, 0.25);\n"
            "\tts.y += DustWarp1D(wMap, ts.y, 0.75);\n"
            "\treturn ts;\n"
            "}\n\n"
            "// [Dust] Improved RTWSM shadow filtering (post-warp offsets)\n"
            "float DustRTWShadow(sampler2D sMap, sampler2D wMap, float4x4 shadowMatrix,\n"
            "                     float3 worldPos, float b, float edgeBias, float2 screenPos,\n"
            "                     float3 normal, float dist, float shadowRange) {\n"
            "\tfloat4 sc = mul(shadowMatrix, float4(worldPos, 1));\n"
            "\tfloat2 center = DustGetOffsetLocationS(wMap, sc.xy);\n"
            "\tfloat2 edge = saturate(abs(center - 0.5) * 20 - 9);\n"
            "\tb += edgeBias * (edge.x + edge.y);\n"
            "\tfloat sd = saturate(sc.z);\n"
            "\n"
            + steepBlock +
            // User-toggleable cliff shadow fix: adds a small bias on steep
            // (near-vertical) faces past a smoothly-ramped distance to suppress
            // shadow acne on cliffs. Off by default; enabling at low CliffFixDistance
            // can fade out close-range vertical shadows.
            "\tif (dustCliffFixEnabled > 0.5) {\n"
            "\t\tfloat cf_ny = abs(normal.y);\n"
            "\t\tfloat cf_steep = saturate((0.42 - cf_ny) * 4.25);\n"
            "\t\tfloat cf_gate = saturate((dist - shadowRange * dustCliffFixDistance) * 0.0035);\n"
            "\t\tb += (cf_steep * cf_steep) * cf_gate * 0.0032;\n"
            "\t}\n"
            "\n"
            "\tfloat noise = frac(52.9829189 * frac(dot(screenPos, float2(0.06711056, 0.00583715))));\n"
            "\tfloat ang = noise * 6.28318530718;\n"
            "\tfloat sa, ca;\n"
            "\tsincos(ang, sa, ca);\n"
            "\tfloat2x2 rot = float2x2(ca, sa, -sa, ca);\n"
            "\n"
            "\tstatic const float2 pd[12] = {\n"
            "\t\tfloat2(-0.326212, -0.405810),\n"
            "\t\tfloat2(-0.840144, -0.073580),\n"
            "\t\tfloat2(-0.695914,  0.457137),\n"
            "\t\tfloat2(-0.203345,  0.620716),\n"
            "\t\tfloat2( 0.962340, -0.194983),\n"
            "\t\tfloat2( 0.473434, -0.480026),\n"
            "\t\tfloat2( 0.519456,  0.767022),\n"
            "\t\tfloat2( 0.185461, -0.893124),\n"
            "\t\tfloat2( 0.507431,  0.064425),\n"
            "\t\tfloat2( 0.896420,  0.412458),\n"
            "\t\tfloat2(-0.321940, -0.932615),\n"
            "\t\tfloat2(-0.791559, -0.597705)\n"
            "\t};\n"
            "\n"
            "\tfloat fr = dustFilterRadius;\n"
            "\tfloat ls = dustLightSize;\n"
            "\tb += fr * dustBiasScale;\n"
            "\n"
            "\tif (dustPCSSEnabled > 0.5) {\n"
            "\t\tfloat bSum = 0;\n"
            "\t\tfloat bCnt = 0;\n"
            "\t\t[unroll]\n"
            "\t\tfor (int j = 0; j < 12; j++) {\n"
            "\t\t\tfloat2 off = mul(rot, pd[j]) * ls;\n"
            "\t\t\tfloat2 suv = center + off;\n"
            "\t\t\tfloat dd = tex2Dlod(sMap, float4(suv, 0, 0)).x;\n"
            "\t\t\tif (dd < sd - b) {\n"
            "\t\t\t\tbSum += dd;\n"
            "\t\t\t\tbCnt += 1.0;\n"
            "\t\t\t}\n"
            "\t\t}\n"
            "\t\tif (bCnt > 0) {\n"
            "\t\t\tfloat avgB = bSum / bCnt;\n"
            "\t\t\tfloat pen = (sd - avgB) * ls / max(avgB, 0.001);\n"
            "\t\t\tfr = clamp(pen, fr * 0.5, fr * 3.0);\n"
            "\t\t}\n"
            "\t}\n"
            "\n"
            "\tfloat3 ld = normalize(shadowMatrix[2].xyz);\n"
            "\tfloat NdotL = abs(dot(normal, ld));\n"
            "\tfr *= max(sqrt(NdotL), 0.15);\n"
            "\n"
            "\tfloat shadow = 0;\n"
            "\t[unroll]\n"
            "\tfor (int i = 0; i < 12; i++) {\n"
            "\t\tfloat2 off = mul(rot, pd[i]) * fr;\n"
            "\t\tfloat2 suv = center + off;\n"
            "\t\tshadow += ShadowMap(sMap, suv, sd, b, 0);\n"
            "\t}\n"
            "\tshadow /= 12.0;\n"
            "\treturn shadow;\n"
            "}\n\n"
            // CSM filter wrapper: scales the per-cascade PCF filter radius
            // (csmParams[i][1] = kCsmPcfFilterRadius) by user-controllable
            // dustCsmFilterScale before forwarding to vanilla. Cascade
            // selection, sampleProj basis, and PCF kernel are vanilla — only
            // the radius changes. Disabled flag falls back to scale = 1.0.
            "float DustCascadeShadow(\n"
            "\tfloat4 shadowParams,\n"
            "\tfloat4x4 shadowViewMat,\n"
            "\tfloat4 csmScale[SHADOW_MAP_COUNT],\n"
            "\tfloat4 csmTrans[SHADOW_MAP_COUNT],\n"
            "\tfloat4 csmParams[SHADOW_MAP_COUNT],\n"
            "\tfloat4 csmUvBounds[SHADOW_MAP_COUNT],\n"
            "\tsampler2D shadowDepthMap,\n"
            "\tsampler2D shadowJitterMap,\n"
            "\tfloat4 posWs,\n"
            "\tfloat4 posSs,\n"
            "\tfloat3 normalWs,\n"
            "\tout float3 debugColorMask)\n"
            "{\n"
            "\tfloat radiusScale = (dustShadowEnabled > 0.5) ? dustCsmFilterScale : 1.0;\n"
            "\tfloat4 modCsmParams[SHADOW_MAP_COUNT];\n"
            "\t[unroll]\n"
            "\tfor (int i = 0; i < SHADOW_MAP_COUNT; i++) {\n"
            "\t\tmodCsmParams[i] = csmParams[i];\n"
            "\t\tmodCsmParams[i][1] *= radiusScale;\n"
            "\t}\n"
            "\treturn computeShadowMultiplier(\n"
            "\t\tshadowParams, shadowViewMat,\n"
            "\t\tcsmScale, csmTrans, modCsmParams, csmUvBounds,\n"
            "\t\tshadowDepthMap, shadowJitterMap,\n"
            "\t\tposWs, posSs, normalWs, debugColorMask);\n"
            "}\n\n";
        result.insert(pos3, inject3);
        Log("ShaderPatch: injected DustShadowParams cbuffer + DustRTWShadow + DustCascadeShadow passthrough");
    }
    else
    {
        Log("ShaderPatch: anchor 'main_vs' not found, shadow function injection skipped");
    }

    // Injection 4: Replace RTWShadow call with conditional.
    // Search for "= RTWShadow(" to find the call site (skips DustRTWShadow definition).
    // Extracts parameters dynamically so it works regardless of spacing or extra bias terms.
    const char* callAnchor = "= RTWShadow(";
    size_t anchorPos = result.find(callAnchor);
    if (anchorPos != std::string::npos)
    {
        size_t funcStart = anchorPos + 2; // position of 'R' in RTWShadow
        size_t openParen = result.find('(', funcStart);

        int depth = 1;
        size_t scan = openParen + 1;
        while (scan < result.size() && depth > 0)
        {
            if (result[scan] == '(') depth++;
            else if (result[scan] == ')') depth--;
            scan++;
        }
        size_t closeParen = scan - 1;

        std::string originalCall = result.substr(funcStart, closeParen - funcStart + 1);
        std::string params = result.substr(openParen + 1, closeParen - openParen - 1);

        std::string newExpr =
            "(dustShadowEnabled > 0.5) "
            "? DustRTWShadow(" + params + ", pixel.xy, normal, distance, shadow_range) "
            ": " + originalCall;

        result.replace(funcStart, closeParen - funcStart + 1, newExpr);
        Log("ShaderPatch: redirected RTWShadow -> conditional DustRTWShadow");
        Log("ShaderPatch: original call: %s", originalCall.c_str());
    }
    else
    {
        Log("ShaderPatch: '= RTWShadow(' not found, shadow redirect skipped");
    }

    // CSM passthrough redirect: replace `shadow = computeShadowMultiplier(`
    // with `shadow = DustCascadeShadow(`. The argument list is identical, so
    // this is a pure name swap; no ternary, no out-param-in-ternary issues.
    const char* csmCallAnchor = "shadow = computeShadowMultiplier(";
    size_t csmAnchorPos = result.find(csmCallAnchor);
    if (csmAnchorPos != std::string::npos)
    {
        const char* csmReplacement = "shadow = DustCascadeShadow(";
        result.replace(csmAnchorPos, strlen(csmCallAnchor), csmReplacement);
        Log("ShaderPatch: redirected computeShadowMultiplier -> DustCascadeShadow (passthrough)");
    }
    else
    {
        Log("ShaderPatch: '= computeShadowMultiplier(' not found, CSM redirect skipped");
    }

    return result;
}

// Patch vanilla objects.hlsl to fix foliage alpha threshold instability.
// Replaces the hard binary clip with Bayer-dithered alpha testing and
// stabilizes the threshold uniform against NaN / out-of-range values.
static std::string PatchObjectsShader(const std::string& src)
{
    std::string result = src;

    const char* anchor = "void main_vs(";
    size_t pos = result.find(anchor);
    if (pos == std::string::npos)
    {
        Log("ShaderPatch: objects anchor 'main_vs' not found, skipping");
        return src;
    }

    std::string helpers =
        "// [Dust] Foliage alpha threshold stabilizer\n"
        "float DustStabilizeThreshold(float t)\n"
        "{\n"
        "\tif (!(t == t)) t = 0.30;\n"
        "\tt = clamp(t, 0.02, 0.98);\n"
        "\tconst float CENTER = 0.32;\n"
        "\tconst float MAX_DEV = 0.08;\n"
        "\tif (abs(t - CENTER) > MAX_DEV) t = CENTER;\n"
        "\treturn t;\n"
        "}\n\n"
        "// [Dust] 4x4 ordered dither (Bayer)\n"
        "float DustBayer4x4(float2 fragXY)\n"
        "{\n"
        "\tint2 p = int2(fragXY) & 3;\n"
        "\tfloat4 r0 = float4(0.0, 8.0, 2.0, 10.0);\n"
        "\tfloat4 r1 = float4(12.0, 4.0, 14.0, 6.0);\n"
        "\tfloat4 r2 = float4(3.0, 11.0, 1.0, 9.0);\n"
        "\tfloat4 r3 = float4(15.0, 7.0, 13.0, 5.0);\n"
        "\tfloat v;\n"
        "\tif (p.y == 0) v = r0[p.x];\n"
        "\telse if (p.y == 1) v = r1[p.x];\n"
        "\telse if (p.y == 2) v = r2[p.x];\n"
        "\telse v = r3[p.x];\n"
        "\treturn (v + 0.5) / 16.0;\n"
        "}\n\n";
    result.insert(pos, helpers);

    const char* vanillaClip = "clip(normalTex.a - threshold);";
    size_t clipPos = result.find(vanillaClip);
    if (clipPos != std::string::npos)
    {
        std::string ditherClip =
            "{\n"
            "\t\tconst float FOL_BAND = 0.01;\n"
            "\t\tfloat fol_t = DustStabilizeThreshold(threshold);\n"
            "\t\tfloat fol_d = normalTex.a - fol_t;\n"
            "\t\tif (fol_d >= FOL_BAND) clip(fol_d);\n"
            "\t\telse if (fol_d <= -FOL_BAND) clip(-1.0);\n"
            "\t\telse clip(saturate(fol_d / (2.0 * FOL_BAND) + 0.5) - DustBayer4x4(fragCoord.xy));\n"
            "\t\t}";
        result.replace(clipPos, strlen(vanillaClip), ditherClip);
        Log("ShaderPatch: replaced vanilla alpha test with dithered version");
    }

    const char* vanillaTrans = "(normalTex.a - threshold) / (1.0 - threshold)";
    size_t transPos = result.find(vanillaTrans);
    if (transPos != std::string::npos)
    {
        std::string stabTrans =
            "(normalTex.a - DustStabilizeThreshold(threshold)) / (1.0 - DustStabilizeThreshold(threshold))";
        result.replace(transPos, strlen(vanillaTrans), stabTrans);
        Log("ShaderPatch: stabilized translucency threshold");
    }

    return result;
}

// Patch vanilla objects.hlsl main_ps to add Parallax Occlusion Mapping.
// Derives height from diffuse luminance (no authored height maps required):
// bright = at-surface, dark = recessed. Skips foliage (#ifndef TRANSPARENCY).
// MVP: hardcoded parameters, no depth correction, no LOD fade.
static std::string PatchObjectsShaderForPOM(const std::string& src)
{
    std::string result = src;

    // Helpers go before main_vs (file-scope, available to both VS and PS).
    const char* helperAnchor = "void main_vs(";
    size_t helperPos = result.find(helperAnchor);
    if (helperPos == std::string::npos)
    {
        Log("ShaderPatch[POM]: anchor 'void main_vs(' not found, skipping");
        return src;
    }

    std::string helpers =
        // Parameters bound by POMState at PS slot 8 on GBuffer pass entry.
        "// [Dust] POM parameter cbuffer (bound by host on GBuffer pass entry)\n"
        "cbuffer DustPOMParams : register(b8) {\n"
        "\tfloat4 dustPomCfg;     // x=enabled, y=heightScale, z=threshold, w=thresholdWidth\n"
        "\tfloat4 dustPomSamples; // x=minSamples, y=maxSamples\n"
        "};\n\n"
        "// [Dust] POM derived height. Mip 2 smooths pixel-level luminance noise.\n"
        "// 'bright = deep' convention: bright pixels get displaced (more parallax\n"
        "// motion = perceived closer/raised) while dark pixels anchor.\n"
        "float DustPOMHeight(Texture2D tex, SamplerState samp, float2 uv)\n"
        "{\n"
        "\tfloat lum = dot(tex.SampleLevel(samp, uv, 2).rgb, float3(0.299, 0.587, 0.114));\n"
        "\treturn saturate((lum - dustPomCfg.z) / max(dustPomCfg.w, 1e-3));\n"
        "}\n\n"
        "// [Dust] POM ray march in tangent space.\n"
        "// viewDirTS.z is clamped to avoid the grazing-angle offset blow-up\n"
        "// (the textbook 'P = V.xy / V.z * scale' diverges as V.z -> 0).\n"
        "float2 DustPOM(Texture2D tex, SamplerState samp, float2 uvIn,\n"
        "               float3 viewDirTS, float heightScale,\n"
        "               int minSamples, int maxSamples)\n"
        "{\n"
        "\tif (viewDirTS.z <= 0.001) return uvIn;\n"
        "\t// Defense in depth: hard cap loop bound regardless of caller.\n"
        "\tint   safeMax  = clamp(maxSamples, 4, 64);\n"
        "\tint   safeMin  = clamp(minSamples, 2, safeMax);\n"
        "\tfloat zClamped = max(viewDirTS.z, 0.3);\n"
        "\tfloat numSteps = lerp((float)safeMax, (float)safeMin, abs(viewDirTS.z));\n"
        "\tfloat stepSize = 1.0 / numSteps;\n"
        "\tfloat2 deltaUV = viewDirTS.xy * heightScale / (zClamped * numSteps);\n"
        "\tfloat2 currUV    = uvIn;\n"
        "\tfloat  currLayer = 0.0;\n"
        "\tfloat  currHeight = DustPOMHeight(tex, samp, currUV);\n"
        "\t[loop]\n"
        "\tfor (int i = 0; i < safeMax && currLayer < currHeight; i++)\n"
        "\t{\n"
        "\t\tcurrUV    -= deltaUV;\n"
        "\t\tcurrHeight = DustPOMHeight(tex, samp, currUV);\n"
        "\t\tcurrLayer += stepSize;\n"
        "\t}\n"
        "\t// Cap-exit: ran out of samples without crossing the heightfield. The\n"
        "\t// ray stayed 'deep' the whole march (e.g. uniform bright region with\n"
        "\t// bright=deep convention), so any computed offset is meaningless and\n"
        "\t// causes dark smears where the displaced UV lands on far-away texels.\n"
        "\tif (currLayer < currHeight) return uvIn;\n"
        "\tfloat2 prevUV     = currUV + deltaUV;\n"
        "\tfloat  prevLayer  = currLayer - stepSize;\n"
        "\tfloat  prevHeight = DustPOMHeight(tex, samp, prevUV);\n"
        "\tfloat  afterDepth  = currHeight - currLayer;\n"
        "\tfloat  beforeDepth = prevHeight - prevLayer;\n"
        "\tfloat  denom = afterDepth - beforeDepth;\n"
        "\tfloat  weight = (abs(denom) > 1e-6) ? afterDepth / denom : 0.0;\n"
        "\treturn lerp(currUV, prevUV, weight);\n"
        "}\n\n";
    result.insert(helperPos, helpers);
    Log("ShaderPatch[POM]: injected DustPOM helpers");

    // Inject POM offset before the "// Texture maps" sampling block.
    // Anchor is on its own line so we can insert above it.
    const char* sampleAnchor = "// Texture maps";
    size_t samplePos = result.find(sampleAnchor);
    if (samplePos == std::string::npos)
    {
        Log("ShaderPatch[POM]: anchor '// Texture maps' not found, skipping injection");
        return src;
    }

    std::string pomBlock =
        "// [Dust] POM offset UV (skipped on foliage variants and when disabled).\n"
        "\t// Stricter gate (in [0.5, 1.5]) and value clamps protect against the\n"
        "\t// brief startup window where the host cbuffer at b8 may not yet be\n"
        "\t// bound, so reads return zeros / garbage / NaN.\n"
        "\t// Gradients of the *original* texCoord are computed unconditionally so\n"
        "\t// downstream samples can use SampleGrad. Without that, the per-pixel\n"
        "\t// jump between adjacent fragments' pomTexCoord values trips the GPU's\n"
        "\t// auto-mip-selector and produces regular dark smears.\n"
        "\tfloat2 _pomDdx = ddx(texCoord);\n"
        "\tfloat2 _pomDdy = ddy(texCoord);\n"
        "\tfloat2 pomTexCoord = texCoord;\n"
        "\t#ifndef TRANSPARENCY\n"
        "\tif (dustPomCfg.x > 0.5 && dustPomCfg.x < 1.5)\n"
        "\t{\n"
        "\t\tfloat _pomScale = clamp(dustPomCfg.y, 0.0, 0.1);\n"
        "\t\tint   _pomMin   = (int)clamp(dustPomSamples.x, 2.0, 64.0);\n"
        "\t\tint   _pomMax   = (int)clamp(dustPomSamples.y, 4.0, 64.0);\n"
        "\t\tif (_pomScale > 0.0)\n"
        "\t\t{\n"
        "\t\t\tfloat3 V_world = normalize(cameraPos - worldPos);\n"
        "\t\t\tfloat3 N = normalize(normal);\n"
        // Reconstruct tangent basis from screen-space derivatives of worldPos
        // and texCoord. Robust against non-uniform world matrix scale.
        "\t\t\tfloat3 dp1 = ddx(worldPos);\n"
        "\t\t\tfloat3 dp2 = ddy(worldPos);\n"
        "\t\t\tfloat2 duv1 = ddx(texCoord);\n"
        "\t\t\tfloat2 duv2 = ddy(texCoord);\n"
        "\t\t\tfloat3 dp2perp = cross(dp2, N);\n"
        "\t\t\tfloat3 dp1perp = cross(N, dp1);\n"
        "\t\t\tfloat3 T = dp2perp * duv1.x + dp1perp * duv2.x;\n"
        "\t\t\tfloat3 B = dp2perp * duv1.y + dp1perp * duv2.y;\n"
        "\t\t\tfloat invMax = rsqrt(max(dot(T,T), dot(B,B)));\n"
        "\t\t\tT *= invMax; B *= invMax;\n"
        "\t\t\tfloat3 viewDirTS = float3(dot(V_world, T), dot(V_world, B), dot(V_world, N));\n"
        // Angle-based fade: POM is unreliable at grazing angles (basis becomes
        // degenerate, ray march divergence amplifies). Smoothly ramp from 0 at
        // viewDirTS.z=0.15 to full at viewDirTS.z=0.4 — kills most edge/corner
        // warping where the surface tangent is near the view direction.
        "\t\t\tfloat _pomFade = smoothstep(0.15, 0.4, viewDirTS.z);\n"
        "\t\t\tif (_pomFade > 0.0)\n"
        "\t\t\t{\n"
        "\t\t\t\tpomTexCoord = DustPOM(base_map, sampleState, texCoord, viewDirTS,\n"
        "\t\t\t\t                       _pomScale * _pomFade, _pomMin, _pomMax);\n"
        // Hard cap on absolute offset magnitude. Even when DustPOM finds a
        // 'real' intersection, displacing too far lands the diffuse sample on
        // unrelated texels (the dark-smear residue). Caps the visible damage.
        "\t\t\t\tfloat2 _pomDelta = pomTexCoord - texCoord;\n"
        "\t\t\t\tfloat _pomLen = length(_pomDelta);\n"
        "\t\t\t\tif (_pomLen > 0.04)\n"
        "\t\t\t\t\tpomTexCoord = texCoord + _pomDelta * (0.04 / _pomLen);\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "\t#endif\n"
        "\t";
    result.insert(samplePos, pomBlock);

    // Substitute Kenshi's '.Sample(sampleState, texCoord)' calls with
    // '.SampleGrad(sampleState, pomTexCoord, _pomDdx, _pomDdy)'. SampleGrad with
    // the original texCoord's gradients prevents the auto-mip-selector from
    // aliasing on the displaced UV's per-pixel jumps. Construction grid
    // ('texCoord * scaffoldTiling') and dust noise ('worldPos.xz') don't match
    // and are left alone.
    const std::string fromPattern = ".Sample(sampleState, texCoord)";
    const std::string toPattern   = ".SampleGrad(sampleState, pomTexCoord, _pomDdx, _pomDdy)";
    size_t scan = samplePos;
    int subCount = 0;
    while ((scan = result.find(fromPattern, scan)) != std::string::npos)
    {
        result.replace(scan, fromPattern.size(), toPattern);
        scan += toPattern.size();
        subCount++;
    }
    Log("ShaderPatch[POM]: injected POM block, substituted %d sample sites", subCount);

    return result;
}

// Diagnostic: log the compiled shader's resource binding layout (cbuffer
// slots, sampler slots, texture slots). This is ground truth for which
// registers are actually used — no guessing about whether a slot is free
// vs. occupied.
static void LogPatchedShaderReflection(const void* bytecode, SIZE_T bytecodeSize)
{
    static int sCounter = 0;
    int idx = sCounter++;

    if (!bytecode || bytecodeSize == 0) return;

    ID3D11ShaderReflection* reflector = nullptr;
    if (FAILED(D3DReflect(bytecode, bytecodeSize, IID_ID3D11ShaderReflection, (void**)&reflector)) ||
        !reflector)
        return;

    D3D11_SHADER_DESC desc = {};
    reflector->GetDesc(&desc);
    Log("Patched shader %d: %u cbuffers, %u bound resources, %u instructions",
        idx, desc.ConstantBuffers, desc.BoundResources, desc.InstructionCount);

    for (UINT i = 0; i < desc.BoundResources; i++)
    {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        if (FAILED(reflector->GetResourceBindingDesc(i, &bd))) continue;

        const char* typeStr = "?";
        switch (bd.Type)
        {
            case D3D_SIT_CBUFFER:   typeStr = "cbuffer";   break;
            case D3D_SIT_TBUFFER:   typeStr = "tbuffer";   break;
            case D3D_SIT_TEXTURE:   typeStr = "texture";   break;
            case D3D_SIT_SAMPLER:   typeStr = "sampler";   break;
            default: break;
        }
        Log("  [%2u] %-16s slot=%2u count=%u  %s",
            i, typeStr, bd.BindPoint, bd.BindCount, bd.Name ? bd.Name : "?");
    }

    // Walk cbuffer variables to show array element counts. Tells us things
    // like the actual SHADOW_MAP_COUNT (= csmParams[].Elements) without
    // having to guess from the source.
    for (UINT i = 0; i < desc.ConstantBuffers; i++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = reflector->GetConstantBufferByIndex(i);
        if (!cb) continue;
        D3D11_SHADER_BUFFER_DESC cbDesc = {};
        if (FAILED(cb->GetDesc(&cbDesc))) continue;
        Log("  cbuffer '%s': %u vars, %u bytes", cbDesc.Name, cbDesc.Variables, cbDesc.Size);
        for (UINT j = 0; j < cbDesc.Variables; j++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
            if (!var) continue;
            D3D11_SHADER_VARIABLE_DESC vDesc = {};
            if (FAILED(var->GetDesc(&vDesc))) continue;
            ID3D11ShaderReflectionType* type = var->GetType();
            D3D11_SHADER_TYPE_DESC tDesc = {};
            if (type) type->GetDesc(&tDesc);
            Log("    %-24s offset=%4u size=%4u elements=%u",
                vDesc.Name ? vDesc.Name : "?", vDesc.StartOffset, vDesc.Size, tDesc.Elements);
        }
    }

    reflector->Release();
}

HRESULT WINAPI HookedD3DCompile(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs)
{
    if (D3D11Hook::IsShutdownSignaled())
        return oD3DCompile(pSrcData, SrcDataSize, pSourceName,
                            pDefines, pInclude, pEntrypoint, pTarget,
                            Flags1, Flags2, ppCode, ppErrorMsgs);

    // Detect the deferred lighting pixel shader: entry point is "main_fs"
    // and source contains deferred-specific identifiers.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_fs") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        if (src.find("CalcEnvironmentLight") != std::string::npos &&
            src.find("aoMap") == std::string::npos)  // not already patched
        {
            std::string patched = PatchDeferredShader(src);
            if (patched.size() != src.size())
            {
                Log("ShaderPatch: patched deferred main_fs (%zu -> %zu bytes)",
                    src.size(), patched.size());
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, pTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    // Record shader source for survey (use patched source)
                    if (ppCode && *ppCode)
                    {
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName,
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                        LogPatchedShaderReflection((*ppCode)->GetBufferPointer(),
                            (*ppCode)->GetBufferSize());
                    }
                    return hr;
                }

                Log("ShaderPatch: patched shader failed to compile, falling back to original");
                if (ppErrorMsgs && *ppErrorMsgs)
                {
                    Log("ShaderPatch: error: %s", (const char*)(*ppErrorMsgs)->GetBufferPointer());
                    (*ppErrorMsgs)->Release();
                    *ppErrorMsgs = nullptr;
                }
                // Fall through to compile original below
            }
        }
    }

    // Detect objects shader: entry point is "main_ps" and source matches the
    // file-level identifier. Apply foliage alpha fix and POM injection in turn —
    // each sub-patch is gated by its own anchors and "already patched" check.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_ps") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        bool isObjects = src.find("// General objects deferred lighting shader") != std::string::npos;
        if (isObjects)
        {
            std::string patched = src;

            if (patched.find("clip(normalTex.a - threshold)") != std::string::npos &&
                patched.find("DustStabilizeThreshold") == std::string::npos)
            {
                patched = PatchObjectsShader(patched);
            }

            if (patched.find("DustPOM") == std::string::npos)
            {
                patched = PatchObjectsShaderForPOM(patched);
            }

            if (patched.size() != src.size())
            {
                Log("ShaderPatch: patched objects main_ps (%zu -> %zu bytes)",
                    src.size(), patched.size());
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, pTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    if (ppCode && *ppCode)
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName,
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                    return hr;
                }

                Log("ShaderPatch: patched objects shader failed to compile, falling back");
                if (ppErrorMsgs && *ppErrorMsgs)
                {
                    Log("ShaderPatch: error: %s", (const char*)(*ppErrorMsgs)->GetBufferPointer());
                    (*ppErrorMsgs)->Release();
                    *ppErrorMsgs = nullptr;
                }
            }
        }
    }

    HRESULT hr = oD3DCompile(pSrcData, SrcDataSize, pSourceName,
                              pDefines, pInclude, pEntrypoint, pTarget,
                              Flags1, Flags2, ppCode, ppErrorMsgs);

    // Record shader source for survey (always, for all shaders)
    if (SUCCEEDED(hr) && ppCode && *ppCode && pSrcData && SrcDataSize > 0)
    {
        SurveyRecorder::OnShaderCompiled(pSrcData, SrcDataSize,
            pEntrypoint, pTarget, pSourceName,
            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
    }

    return hr;
}

} // namespace ShaderPatch
