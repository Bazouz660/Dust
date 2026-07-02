#include "ShaderPatch.h"
#include "CSMCapture.h"
#include "DustLog.h"
#include "SurveyRecorder.h"
#include "D3D11Hook.h"
#include "TerrainTess.h"

#include <tracy/Tracy.hpp>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <string>
#include <cstring>
#include <cstdio>

namespace ShaderPatch
{

PFN_D3DCompileHook oD3DCompile = nullptr;

static char gCompileError[512] = {};

const char* GetLastCompileError() { return gCompileError[0] ? gCompileError : nullptr; }
void        ClearCompileError()   { gCompileError[0] = '\0'; }

// Patch vanilla deferred.hlsl source to add AO support and improved shadow filtering.
// Returns the modified source, or the original if patterns weren't found.
static std::string PatchDeferredShader(const std::string& src)
{
    std::string result = src;

    // === AO Patches ===

    // Injection 1: Global AO texture/sampler declarations (SM5 explicit registers).
    // Explicit register(t8)/register(t9) prevents the HLSL compiler from remapping
    // texture slots — critical because main_fs and light_fs have different sampler
    // counts, and the compiler auto-packs t-registers densely per entry point.
    // Anchor: "void main_vs (" — insert before it so declarations are visible to
    // both main_fs and light_fs.
    const char* aoGlobalAnchor = "void main_vs (";
    size_t aoGlobalPos = result.find(aoGlobalAnchor);
    if (aoGlobalPos == std::string::npos)
    {
        Log("ShaderPatch: anchor 'main_vs' not found, AO + shadow injection skipped");
        return src;
    }
    {
        std::string aoGlobals =
            "// [Dust] AO textures — explicit t-register to prevent compiler remapping\n"
            "Texture2D<float> dustAoTex    : register(t8);\n"
            "Texture2D<float> dustAoParams : register(t9);\n"
            "SamplerState     dustAoSamp   : register(s8);\n"
            // ==================================================================
            // [Dust] Material / BRDF (experimental). Toggles come from a cbuffer
            // bound by the Materials plugin at b10 (DustBrdfParams); the helpers
            // below take their inputs as parameters (no cbuffer reads), so order
            // relative to the cbuffer decl doesn't matter. Each term is gated at
            // the CALL SITE by dustBrdfEnabled + its own toggle.
            // ==================================================================
            // [Dust] Material / BRDF parameters (bound by Materials plugin at b10).
            // Layout must match BrdfCBData in effects/materials/DustMaterials.cpp.
            "cbuffer DustBrdfParams : register(b10) {\n"
            "\tfloat dustBrdfEnabled;\n"
            "\tfloat dustBrdfDisneyDiffuse;\n"
            "\tfloat dustBrdfMultiscatter;\n"
            "\tfloat dustBrdfSpecOcclusion;\n"
            "\tfloat dustBrdfSpecAA;\n"
            "\tfloat dustBrdfStrength;\n"
            "};\n\n"
            // (2) Multiscatter GGX energy compensation.
            // Single-scatter GGX loses energy on rough/metallic surfaces. We add
            // back the energy lost to multiple bounces using the classic
            // Fdez-Aguera / Kulla-Conty form:
            //     spec *= 1 + F0 * (1/Ess - 1)
            // where Ess is the single-scatter directional albedo for white F0.
            // Ess is estimated with Karis' analytic env-BRDF approximation
            // (UE4 'EnvBRDFApprox'): Ess = scale + bias for F0 = 1.
            // rough = GlossToRoughness(gloss); NoV = saturate(dot(normal,viewDir)).
            "float3 DustEnergyComp(float3 specColor, float rough, float NoV) {\n"
            "\tconst float4 c0 = float4(-1.0, -0.0275, -0.572,  0.022);\n"
            "\tconst float4 c1 = float4( 1.0,  0.0425,  1.04,  -0.04);\n"
            "\tfloat4 r = rough * c0 + c1;\n"
            "\tfloat a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;\n"
            "\tfloat2 ab = float2(-1.04, 1.04) * a004 + r.zw;\n"
            "\tfloat Ess = ab.x + ab.y;\n"          // single-scatter directional albedo (white F0)
            "\tEss = clamp(Ess, 1e-3, 1.0);\n"
            "\treturn 1.0 + specColor * (1.0 / Ess - 1.0);\n"
            "}\n\n"
            // (3) Lagarde specular occlusion. AO normally dims only diffuse; this
            // lets ambient specular be occluded in crevices too. Frostbite form.
            "float DustSpecOcc(float NoV, float ao, float rough) {\n"
            // NoV and ao are both saturated upstream, so the base is in [0,2];
            // max(.,0) just silences fxc's generic pow(neg) warning.
            "\treturn saturate(pow(max(NoV + ao, 0.0), exp2(-16.0 * rough - 1.0)) - 1.0 + ao);\n"
            "}\n\n"
            // (4) Geometric specular anti-aliasing (Tokuyoshi/Kaplanyan normal
            // filtering). Widen roughness using the screen-space variance of the
            // GBuffer normal (ddx/ddy of the world normal). Reduces specular
            // shimmer on high-frequency surfaces. Returns a NEW roughness.
            "float DustSpecAARoughness(float3 N, float rough) {\n"
            "\tconst float SIGMA2 = 0.25;\n"     // screen-space variance scale
            "\tconst float KAPPA  = 0.18;\n"     // clamp on added roughness
            "\tfloat3 dndu = ddx(N);\n"
            "\tfloat3 dndv = ddy(N);\n"
            "\tfloat variance = SIGMA2 * (dot(dndu, dndu) + dot(dndv, dndv));\n"
            "\tfloat kernelRough2 = min(2.0 * variance, KAPPA);\n"
            "\tfloat r2 = saturate(rough * rough + kernelRough2);\n"
            "\treturn sqrt(r2);\n"
            "}\n\n"
            // Apply spec AA at the material level by widening GLOSS, so the vanilla
            // GGX (sun + point/spot) and the env-IBL specular mip all inherit the
            // AA'd roughness — not just the injected multiscatter/spec-occ terms.
            // gloss<->roughness uses Kenshi's mapping (lightingFunctions.hlsl):
            //   roughness = 1 - gloss*0.99  ->  gloss = (1 - roughness)/0.99
            "float DustSpecAAGloss(float3 N, float gloss) {\n"
            "\tfloat rough = 1.0 - gloss * 0.99;\n"
            "\trough = DustSpecAARoughness(N, rough);\n"
            "\treturn saturate((1.0 - rough) / 0.99);\n"
            "}\n\n";
        result.insert(aoGlobalPos, aoGlobals);
    }

    // Injection 2: AO application in main_fs (ambient + sun).
    // Anchor: "LightingData ld = (LightingData)0.0f;" — right after env/sun light calculation.
    const char* anchor2 = "LightingData ld = (LightingData)0.0f;";
    size_t pos2 = result.find(anchor2);
    if (pos2 == std::string::npos)
    {
        Log("ShaderPatch: anchor 'LightingData ld' not found, skipping");
        return src;
    }

    std::string inject2 =
        "// [Dust] Ambient occlusion\n"
        "\tfloat ao = dustAoTex.SampleLevel(dustAoSamp, texCoord, 0);\n"
        "\tfloat directAO = dustAoParams.SampleLevel(dustAoSamp, texCoord, 0);\n"
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
            "// Layout must match ShadowCBData in effects/shadows/DustShadows.cpp.\n"
            "cbuffer DustShadowParams : register(b7) {\n"
            "\tfloat dustShadowEnabled;\n"
            "\tfloat dustRtwFilterRadius;\n"
            "\tfloat dustRtwLightSize;\n"
            "\tfloat dustRtwPcssEnabled;\n"
            "\tfloat dustRtwBiasScale;\n"
            "\tfloat dustRtwCliffFixEnabled;\n"
            "\tfloat dustRtwCliffFixDistance;\n"
            "\tfloat dustRtwNormalBias;\n"
            "\tfloat dustRtwSlopeBias;\n"
            "\tfloat dustCsmFilterRadius;\n"
            "\tfloat dustCsmLightSize;\n"
            "\tfloat dustCsmPcssEnabled;\n"
            "\tfloat dustCsmBlendEnabled;\n"
            "\tfloat dustCsmBlendWidth;\n"
            "\tfloat dustRtwQuality;\n"
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
            "// [Dust] tex2Dlod-based shadow compare (safe inside [branch])\n"
            "float DustShadowCmp(sampler2D sm, float2 uv, float d, float bias) {\n"
            "\treturn tex2Dlod(sm, float4(uv, 0, 0)).x >= d - bias ? 1.0 : 0.0;\n"
            "}\n\n"
            "// [Dust] Improved RTWSM shadow filtering (post-warp offsets)\n"
            "float DustRTWShadow(sampler2D sMap, sampler2D wMap, float4x4 shadowMatrix,\n"
            "                     float3 worldPos, float b, float edgeBias, float2 screenPos,\n"
            "                     float3 normal, float dist, float shadowRange) {\n"
            "\tfloat3 ld = normalize(shadowMatrix[2].xyz);\n"
            "\tfloat NdotL = abs(dot(normal, ld));\n"
            "\n"
            "\tfloat3 lookupPos = worldPos + normal * (dustRtwNormalBias * (1.0 - NdotL));\n"
            "\tfloat4 sc = mul(shadowMatrix, float4(lookupPos, 1));\n"
            "\tfloat2 center = DustGetOffsetLocationS(wMap, sc.xy);\n"
            "\tfloat2 edge = saturate(abs(center - 0.5) * 20 - 9);\n"
            "\tb += edgeBias * (edge.x + edge.y);\n"
            "\tfloat sd = saturate(mul(shadowMatrix, float4(worldPos, 1)).z);\n"
            "\n"
            "\tfloat sinSlope = sqrt(1.0 - NdotL * NdotL);\n"
            "\tb += dustRtwSlopeBias * sinSlope / max(NdotL, 0.01);\n"
            "\n"
            + steepBlock +
            "\tif (dustRtwCliffFixEnabled > 0.5) {\n"
            "\t\tfloat cf_ny = abs(normal.y);\n"
            "\t\tfloat cf_steep = saturate((0.42 - cf_ny) * 4.25);\n"
            "\t\tfloat cf_gate = saturate((dist - shadowRange * dustRtwCliffFixDistance) * 0.0035);\n"
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
            "\tfloat fr = dustRtwFilterRadius;\n"
            "\tfloat ls = dustRtwLightSize;\n"
            "\tb += fr * dustRtwBiasScale;\n"
            "\n"
            // Early-out: most pixels are fully lit. 3 taps catches them.
            "\tfloat centerD = tex2Dlod(sMap, float4(center, 0, 0)).x;\n"
            "\t[branch] if (centerD >= sd - b) {\n"
            "\t\tfloat d0 = tex2Dlod(sMap, float4(center + mul(rot, pd[0]) * fr, 0, 0)).x;\n"
            "\t\tfloat d6 = tex2Dlod(sMap, float4(center + mul(rot, pd[6]) * fr, 0, 0)).x;\n"
            "\t\tif (d0 >= sd - b && d6 >= sd - b) return 1.0;\n"
            "\t}\n"
            "\n"
            // PCSS blocker search with 4-tap probe early-exit
            "\tif (dustRtwPcssEnabled > 0.5) {\n"
            "\t\tfloat bSum = 0;\n"
            "\t\tfloat bCnt = 0;\n"
            "\t\t[unroll]\n"
            "\t\tfor (int j = 0; j < 4; j++) {\n"
            "\t\t\tfloat dd = tex2Dlod(sMap, float4(center + mul(rot, pd[j]) * ls, 0, 0)).x;\n"
            "\t\t\tif (dd < sd - b) { bSum += dd; bCnt += 1.0; }\n"
            "\t\t}\n"
            "\t\t[branch] if (bCnt > 0) {\n"
            "\t\t\t[unroll]\n"
            "\t\t\tfor (int j = 4; j < 12; j++) {\n"
            "\t\t\t\tfloat dd = tex2Dlod(sMap, float4(center + mul(rot, pd[j]) * ls, 0, 0)).x;\n"
            "\t\t\t\tif (dd < sd - b) { bSum += dd; bCnt += 1.0; }\n"
            "\t\t\t}\n"
            "\t\t\tfloat avgB = bSum / bCnt;\n"
            "\t\t\tfloat pen = (sd - avgB) * ls / max(avgB, 0.001);\n"
            "\t\t\tfr = clamp(pen, fr * 0.5, fr * 3.0);\n"
            "\t\t}\n"
            "\t}\n"
            "\n"
            "\tfr *= max(sqrt(NdotL), 0.15);\n"
            "\n"
            // Resolution-tiered PCF: 4/8/12 taps via uniform branches.
            "\tfloat shadow = 0;\n"
            "\t[unroll] for (int i = 0; i < 4; i++)\n"
            "\t\tshadow += DustShadowCmp(sMap, center + mul(rot, pd[i]) * fr, sd, b);\n"
            "\tfloat sCount = 4.0;\n"
            "\t[branch] if (dustRtwQuality > 4.5) {\n"
            "\t\t[unroll] for (int i = 4; i < 8; i++)\n"
            "\t\t\tshadow += DustShadowCmp(sMap, center + mul(rot, pd[i]) * fr, sd, b);\n"
            "\t\tsCount = 8.0;\n"
            "\t\t[branch] if (dustRtwQuality > 8.5) {\n"
            "\t\t\t[unroll] for (int i = 8; i < 12; i++)\n"
            "\t\t\t\tshadow += DustShadowCmp(sMap, center + mul(rot, pd[i]) * fr, sd, b);\n"
            "\t\t\tsCount = 12.0;\n"
            "\t\t}\n"
            "\t}\n"
            "\tshadow /= sCount;\n"
            "\treturn shadow;\n"
            "}\n\n"
            // CSM Poisson disk for blocker search + PCF. Reuse the same 12-tap
            // table as DustRTWShadow; values are pre-normalized to length ~1.
            "static const float2 kDustCsmPoisson[12] = {\n"
            "\tfloat2(-0.326212, -0.405810),\n"
            "\tfloat2(-0.840144, -0.073580),\n"
            "\tfloat2(-0.695914,  0.457137),\n"
            "\tfloat2(-0.203345,  0.620716),\n"
            "\tfloat2( 0.962340, -0.194983),\n"
            "\tfloat2( 0.473434, -0.480026),\n"
            "\tfloat2( 0.519456,  0.767022),\n"
            "\tfloat2( 0.185461, -0.893124),\n"
            "\tfloat2( 0.507431,  0.064425),\n"
            "\tfloat2( 0.896420,  0.412458),\n"
            "\tfloat2(-0.321940, -0.932615),\n"
            "\tfloat2(-0.791559, -0.597705)\n"
            "};\n\n"

            // Sample a single cascade with Poisson PCF + optional PCSS.
            //
            // Two specializations:
            //   DustSampleCascade8 — 8 taps, 4-tap PCSS early-exit. Used for
            //                       cascades 0..N-2 (near + mid).
            //   DustSampleCascade4 — 4 taps, 2-tap PCSS early-exit. Used for
            //                       the far cascade where texels are huge
            //                       and extra samples only blur noise.
            //
            // Both specializations:
            //   - PCSS blocker search does a half-count probe first, exits if
            //     no blockers found. Wave-uniform when the surface is fully
            //     lit (most pixels in the common case), saving ~half the
            //     blocker search cost on those waves.
            //   - sampleProj is the vanilla surface-aligned basis kept so the
            //     filter footprint stays tangent to the lit surface — same
            //     acne suppression the vanilla PCF gets.
            //   - pcfSample (from shadowFunctions.hlsl) returns
            //     storedDepth - receiverDepth, so a negative result means
            //     this sample is occluded.
            "float DustSampleCascade8(\n"
            "\tsampler2D shadowDepthMap, sampler2D jitterMap,\n"
            "\tfloat3 shadowUv, float3x3 sampleProj, float baseRadius)\n"
            "{\n"
            "\tfloat2 noise = tex2Dlod(jitterMap, float4(shadowUv.xy * 1024.0, 0, 0)).xy;\n"
            "\tfloat sa, ca;\n"
            "\tsincos(noise.x * 6.28318530718, sa, ca);\n"
            "\tfloat2x2 rotBase = float2x2(ca, sa, -sa, ca);\n"
            "\n"
            "\tfloat radius = baseRadius;\n"
            "\tif (dustCsmPcssEnabled > 0.5) {\n"
            "\t\tfloat searchR = baseRadius * dustCsmLightSize;\n"
            "\t\tfloat2x2 searchRot = rotBase * searchR;\n"
            "\t\tfloat blockerDeltaSum = 0;\n"
            "\t\tfloat blockerCnt = 0;\n"
            // Probe with first 4 taps; if no blockers, surface is fully lit
            // and we can skip the remaining 4 taps entirely.
            "\t\t[unroll]\n"
            "\t\tfor (int j = 0; j < 4; j++) {\n"
            "\t\t\tfloat d = pcfSample(shadowDepthMap, shadowUv, sampleProj, searchRot, kDustCsmPoisson[j]);\n"
            "\t\t\tif (d < 0) { blockerDeltaSum -= d; blockerCnt += 1; }\n"
            "\t\t}\n"
            "\t\tif (blockerCnt > 0) {\n"
            "\t\t\t[unroll]\n"
            "\t\t\tfor (int j = 4; j < 8; j++) {\n"
            "\t\t\t\tfloat d = pcfSample(shadowDepthMap, shadowUv, sampleProj, searchRot, kDustCsmPoisson[j]);\n"
            "\t\t\t\tif (d < 0) { blockerDeltaSum -= d; blockerCnt += 1; }\n"
            "\t\t\t}\n"
            "\t\t\tfloat avgDelta = blockerDeltaSum / blockerCnt;\n"
            "\t\t\tfloat receiver = max(shadowUv.z, 0.001);\n"
            "\t\t\tfloat pen = (avgDelta / (receiver - avgDelta)) * (dustCsmLightSize * baseRadius);\n"
            "\t\t\tradius = clamp(pen, baseRadius * 0.5, baseRadius * 4.0);\n"
            "\t\t}\n"
            "\t}\n"
            "\n"
            "\tfloat2x2 rotFinal = rotBase * radius;\n"
            "\tfloat shadow = 0;\n"
            "\t[unroll]\n"
            "\tfor (int k = 0; k < 8; k++) {\n"
            "\t\tfloat d = pcfSample(shadowDepthMap, shadowUv, sampleProj, rotFinal, kDustCsmPoisson[k]);\n"
            "\t\tshadow += (d >= 0) ? 1.0 : 0.0;\n"
            "\t}\n"
            "\treturn shadow * (1.0 / 8.0);\n"
            "}\n\n"

            "float DustSampleCascade4(\n"
            "\tsampler2D shadowDepthMap, sampler2D jitterMap,\n"
            "\tfloat3 shadowUv, float3x3 sampleProj, float baseRadius)\n"
            "{\n"
            "\tfloat2 noise = tex2Dlod(jitterMap, float4(shadowUv.xy * 1024.0, 0, 0)).xy;\n"
            "\tfloat sa, ca;\n"
            "\tsincos(noise.x * 6.28318530718, sa, ca);\n"
            "\tfloat2x2 rotBase = float2x2(ca, sa, -sa, ca);\n"
            "\n"
            "\tfloat radius = baseRadius;\n"
            "\tif (dustCsmPcssEnabled > 0.5) {\n"
            "\t\tfloat searchR = baseRadius * dustCsmLightSize;\n"
            "\t\tfloat2x2 searchRot = rotBase * searchR;\n"
            "\t\tfloat blockerDeltaSum = 0;\n"
            "\t\tfloat blockerCnt = 0;\n"
            "\t\t[unroll]\n"
            "\t\tfor (int j = 0; j < 2; j++) {\n"
            "\t\t\tfloat d = pcfSample(shadowDepthMap, shadowUv, sampleProj, searchRot, kDustCsmPoisson[j]);\n"
            "\t\t\tif (d < 0) { blockerDeltaSum -= d; blockerCnt += 1; }\n"
            "\t\t}\n"
            "\t\tif (blockerCnt > 0) {\n"
            "\t\t\t[unroll]\n"
            "\t\t\tfor (int j = 2; j < 4; j++) {\n"
            "\t\t\t\tfloat d = pcfSample(shadowDepthMap, shadowUv, sampleProj, searchRot, kDustCsmPoisson[j]);\n"
            "\t\t\t\tif (d < 0) { blockerDeltaSum -= d; blockerCnt += 1; }\n"
            "\t\t\t}\n"
            "\t\t\tfloat avgDelta = blockerDeltaSum / blockerCnt;\n"
            "\t\t\tfloat receiver = max(shadowUv.z, 0.001);\n"
            "\t\t\tfloat pen = (avgDelta / (receiver - avgDelta)) * (dustCsmLightSize * baseRadius);\n"
            "\t\t\tradius = clamp(pen, baseRadius * 0.5, baseRadius * 4.0);\n"
            "\t\t}\n"
            "\t}\n"
            "\n"
            "\tfloat2x2 rotFinal = rotBase * radius;\n"
            "\tfloat shadow = 0;\n"
            "\t[unroll]\n"
            "\tfor (int k = 0; k < 4; k++) {\n"
            "\t\tfloat d = pcfSample(shadowDepthMap, shadowUv, sampleProj, rotFinal, kDustCsmPoisson[k]);\n"
            "\t\tshadow += (d >= 0) ? 1.0 : 0.0;\n"
            "\t}\n"
            "\treturn shadow * (1.0 / 4.0);\n"
            "}\n\n"

            // DustCascadeShadow: replaces vanilla computeShadowMultiplier when
            // dustShadowEnabled > 0.5. Falls back to vanilla otherwise so the
            // Shadows plugin's Enabled toggle works.
            // Adds: Poisson PCF, optional PCSS blocker search, smooth blend
            // between adjacent cascades in the band [splitFar - bandWidth, splitFar].
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
            "\tdebugColorMask = float3(1, 1, 1);\n"
            "\tif (dustShadowEnabled < 0.5)\n"
            "\t\treturn computeShadowMultiplier(\n"
            "\t\t\tshadowParams, shadowViewMat,\n"
            "\t\t\tcsmScale, csmTrans, csmParams, csmUvBounds,\n"
            "\t\t\tshadowDepthMap, shadowJitterMap,\n"
            "\t\t\tposWs, posSs, normalWs, debugColorMask);\n"
            "\n"
            // Past the last cascade: no shadow.
            "\tif (posSs.z > csmParams[SHADOW_MAP_COUNT - 1][0])\n"
            "\t\treturn 1.0;\n"
            "\n"
            // Cascade selection — matches vanilla's monotone bucketing.
            // After the loop, idx is the smallest i where posSs.z <= csmParams[i][0].
            "\tint idx = 0;\n"
            "\t[unroll]\n"
            "\tfor (int i = 0; i < SHADOW_MAP_COUNT - 1; i++) {\n"
            "\t\tif (posSs.z > csmParams[i][0]) idx = i + 1;\n"
            "\t}\n"
            "\n"
            // Surface-aligned sample basis (vanilla pattern). Computed once
            // and shared between both cascade samples in the blend band.
            "\tfloat3 normalLs = normalize(mul(shadowViewMat, float4(normalWs, 0)).xyz);\n"
            "\tfloat3 xDir = float3(1, 0, 0) - normalLs.x * normalLs;\n"
            "\tfloat3 yDir = float3(0, 1, 0) - normalLs.y * normalLs;\n"
            "\tfloat3 zDir = float3(0, 0, 1) - normalLs.z * normalLs;\n"
            "\tfloat3x3 sampleProj = float3x3(xDir, yDir, zDir);\n"
            "\n"
            "\tfloat3 posLs = mul(shadowViewMat, posWs).xyz;\n"
            "\n"
            // Sample primary cascade. Far cascade uses the cheaper 4-tap
            // path: huge texels mean extra samples mostly blur noise.
            "\tfloat3 shadowUv0 = csmTrans[idx].xyz + csmScale[idx].xyz * posLs;\n"
            "\tfloat baseRadius0 = csmParams[idx][1] * dustCsmFilterRadius;\n"
            "\tfloat s0 = (idx >= SHADOW_MAP_COUNT - 1)\n"
            "\t\t? DustSampleCascade4(shadowDepthMap, shadowJitterMap, shadowUv0, sampleProj, baseRadius0)\n"
            "\t\t: DustSampleCascade8(shadowDepthMap, shadowJitterMap, shadowUv0, sampleProj, baseRadius0);\n"
            "\n"
            "\tfloat shadowMul = s0;\n"
            "\n"
            // Cascade blending. Activate inside the band approaching this
            // cascade's far split. Sample the next cascade only when the
            // band is actually entered (uniform branch in screen tiles).
            // Next cascade is always farther — if next is the last one, use
            // the cheap 4-tap path; otherwise the 8-tap.
            "\tif (dustCsmBlendEnabled > 0.5 && idx + 1 < SHADOW_MAP_COUNT) {\n"
            "\t\tfloat splitFar  = csmParams[idx][0];\n"
            "\t\tfloat splitNear = (idx > 0) ? csmParams[idx - 1][0] : 0.0;\n"
            "\t\tfloat band      = max((splitFar - splitNear) * dustCsmBlendWidth, 1e-5);\n"
            "\t\tfloat blendT    = saturate((posSs.z - (splitFar - band)) / band);\n"
            "\t\tif (blendT > 0.0) {\n"
            "\t\t\tfloat3 shadowUv1 = csmTrans[idx + 1].xyz + csmScale[idx + 1].xyz * posLs;\n"
            "\t\t\tfloat baseRadius1 = csmParams[idx + 1][1] * dustCsmFilterRadius;\n"
            "\t\t\tfloat s1 = (idx + 1 >= SHADOW_MAP_COUNT - 1)\n"
            "\t\t\t\t? DustSampleCascade4(shadowDepthMap, shadowJitterMap, shadowUv1, sampleProj, baseRadius1)\n"
            "\t\t\t\t: DustSampleCascade8(shadowDepthMap, shadowJitterMap, shadowUv1, sampleProj, baseRadius1);\n"
            "\t\t\tshadowMul = lerp(s0, s1, blendT);\n"
            "\t\t}\n"
            "\t}\n"
            "\n"
            // Vanilla ambient-floor pass: shadow value is lifted by the
            // ambient term so unlit areas stay above shadowAmbient.
            "\tfloat shadowAmbient = shadowParams[2];\n"
            "\tshadowMul = shadowAmbient + shadowMul * (1.0 - shadowAmbient);\n"
            "\treturn shadowMul;\n"
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

    // Injection 5: AO application in light_fs (point lights / spotlights).
    // Anchor: "color = color * attenuation * power;" — unique to light_fs.
    const char* lightAnchor = "color = color * attenuation * power;";
    size_t lightPos = result.find(lightAnchor);
    if (lightPos != std::string::npos)
    {
        size_t insertPos = lightPos + strlen(lightAnchor);
        std::string lightAO =
            "\n\t// [Dust] Direct light AO\n"
            "\t{\n"
            "\t\tfloat _ao = dustAoTex.SampleLevel(dustAoSamp, texCoord, 0);\n"
            "\t\tfloat _dAO = dustAoParams.SampleLevel(dustAoSamp, texCoord, 0);\n"
            "\t\tcolor *= lerp(1.0, _ao, _dAO);\n"
            "\t}\n";
        result.insert(insertPos, lightAO);

        Log("ShaderPatch: injected AO into light_fs");
    }
    else
    {
        Log("ShaderPatch: 'color = color * attenuation * power;' not found, light_fs AO skipped");
    }

    // ========================================================================
    // [Dust] Geometric specular AA. Widen GLOSS after it is decoded from the
    // GBuffer but BEFORE the vanilla specular is evaluated (CalcPunctualLight /
    // CalcEnvironmentLight), so the sun GGX, point/spot GGX and env-IBL specular
    // are all anti-aliased. Standalone: gated by dustBrdfSpecAA alone (the master
    // dustBrdfEnabled is NOT required), so AA works without the experimental BRDF.
    // ========================================================================
    {
        // main_fs: sun + ambient. gloss (gBuf0.a) and normal (gBuf1) are in scope.
        const char* specAASunAnchor = "LightingData sunLight = CalcPunctualLight";
        size_t specAASunPos = result.find(specAASunAnchor);
        if (specAASunPos != std::string::npos)
        {
            std::string inj =
                "[branch] if (dustBrdfSpecAA > 0.5)\n"
                "\t\tgloss = DustSpecAAGloss(normal, gloss);\n\t";
            result.insert(specAASunPos, inj);
            Log("ShaderPatch: injected spec-AA gloss widen into main_fs");
        }

        // light_fs: point/spot. Same gloss/normal decode upstream.
        const char* specAALightAnchor = "LightingData ld = CalcPunctualLight";
        size_t specAALightPos = result.find(specAALightAnchor);
        if (specAALightPos != std::string::npos)
        {
            std::string inj =
                "[branch] if (dustBrdfSpecAA > 0.5)\n"
                "\t\tgloss = DustSpecAAGloss(normal, gloss);\n\t";
            result.insert(specAALightPos, inj);
            Log("ShaderPatch: injected spec-AA gloss widen into light_fs");
        }
    }

    // ========================================================================
    // [Dust] Better-BRDF per-term modifications. All gated by dustBrdfEnabled
    // plus each term's own toggle. main_fs has sunLight/envLight; light_fs has
    // only ld. Each block guards on whether its anchor exists, so this runs
    // safely for both entry points (PatchDeferredShader is called for each).
    // ========================================================================

    // --- main_fs: sun + ambient. Anchor on the ld.diffuse accumulate line.
    // In scope here: normal, viewDir, lightDir, gloss, specColor, sunLight,
    // envLight, and the injected ao (from AO injection 2 above).
    {
        const char* brdfMainAnchor = "ld.diffuse = sunLight.diffuse + envLight.diffuse;";
        size_t brdfMainPos = result.find(brdfMainAnchor);
        if (brdfMainPos != std::string::npos)
        {
            std::string brdfMain =
                "// [Dust] Better-BRDF (sun + ambient). All gated by dustBrdfEnabled.\n"
                "\t[branch] if (dustBrdfEnabled > 0.5) {\n"
                "\t\tfloat _brdfNoV   = saturate(dot(normal, viewDir));\n"
                // gloss is already spec-AA-widened upstream (if dustBrdfSpecAA is on),
                // so _brdfRough inherits the AA'd roughness for the terms below.
                "\t\tfloat _brdfRough = GlossToRoughness(gloss);\n"
                // (1) Disney diffuse: swap the cheap constant FresnelDiffuse for the
                // Burley angular response, preserving the existing N.L / translucency.
                "\t\t[branch] if (dustBrdfDisneyDiffuse > 0.5) {\n"
                "\t\t\tfloat _dd = Fr_DisneyDiffuse(viewDir, lightDir, normal, _brdfRough);\n"
                "\t\t\tsunLight.diffuse *= lerp(1.0, _dd / max(FresnelDiffuse(specColor).x, 1e-3), dustBrdfStrength);\n"
                "\t\t}\n"
                // (2) Multiscatter energy compensation on specular (sun + ambient).
                "\t\t[branch] if (dustBrdfMultiscatter > 0.5) {\n"
                "\t\t\tfloat3 _ec = lerp(float3(1.0,1.0,1.0), DustEnergyComp(specColor, _brdfRough, _brdfNoV), dustBrdfStrength);\n"
                "\t\t\tsunLight.specular *= _ec;\n"
                "\t\t\tenvLight.specular *= _ec;\n"
                "\t\t}\n"
                // (3) Specular occlusion on ambient specular (uses injected ao).
                "\t\t[branch] if (dustBrdfSpecOcclusion > 0.5) {\n"
                "\t\t\tenvLight.specular *= lerp(1.0, DustSpecOcc(_brdfNoV, ao, _brdfRough), dustBrdfStrength);\n"
                "\t\t}\n"
                "\t}\n\t";
            result.insert(brdfMainPos, brdfMain);
            Log("ShaderPatch: injected Better-BRDF terms into main_fs");
        }

        // --- light_fs: point/spot. Anchor on the color accumulate line. In scope:
        // normal, viewDir, lightDir, gloss, specColor, ld, texCoord. No ambient
        // specular term here, so spec-occlusion (an ambient-only term) is N/A.
        const char* brdfLightAnchor = "float3 color = ld.specular + ld.diffuse * albedo;";
        size_t brdfLightPos = result.find(brdfLightAnchor);
        if (brdfLightPos != std::string::npos)
        {
            std::string brdfLight =
                "// [Dust] Better-BRDF (point/spot). Gated by dustBrdfEnabled.\n"
                "\t[branch] if (dustBrdfEnabled > 0.5) {\n"
                "\t\tfloat _brdfNoV   = saturate(dot(normal, viewDir));\n"
                "\t\tfloat _brdfRough = GlossToRoughness(gloss);\n"
                // (1) Disney diffuse.
                "\t\t[branch] if (dustBrdfDisneyDiffuse > 0.5) {\n"
                "\t\t\tfloat _dd = Fr_DisneyDiffuse(viewDir, lightDir, normal, _brdfRough);\n"
                "\t\t\tld.diffuse *= lerp(1.0, _dd / max(FresnelDiffuse(specColor).x, 1e-3), dustBrdfStrength);\n"
                "\t\t}\n"
                // (2) Multiscatter energy compensation on direct specular.
                "\t\t[branch] if (dustBrdfMultiscatter > 0.5) {\n"
                "\t\t\tld.specular *= lerp(float3(1.0,1.0,1.0), DustEnergyComp(specColor, _brdfRough, _brdfNoV), dustBrdfStrength);\n"
                "\t\t}\n"
                "\t}\n\t";
            result.insert(brdfLightPos, brdfLight);
            Log("ShaderPatch: injected Better-BRDF terms into light_fs");
        }
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


// Patch terrain main_fs to add a heightmap-as-albedo debug view.
// The heightArray is bound by TerrainTess at PS slot t12, the TessControl
// cbuffer at PS slot b1. When gDebugViewMode > 0.5, the patch overrides
// biome.albedo with the same blended heightmap that drives DS displacement.
static std::string PatchTerrainShaderForHeightDebug(const std::string& src)
{
    std::string result = src;

    const char* anchor1 = "struct BiomeOutput {";
    size_t pos1 = result.find(anchor1);
    if (pos1 == std::string::npos)
    {
        Log("ShaderPatch[TerrainHeightDebug]: anchor 'struct BiomeOutput' not found, skipping");
        return src;
    }

    std::string inject1 =
        "// [Dust] Terrain debug overlay driven by gDebugViewMode (TessControl b1).\n"
        "// Layout MUST match TerrainTess::Controls exactly (24 floats + 3 float4 masks).\n"
        "cbuffer TessControl : register(b1) {\n"
        "\tfloat gMaxFactor; float gFactFadeStart; float gFactFadeEnd; float gAmplitude;\n"
        "\tfloat gAmpFadeStart; float gAmpFadeEnd; float gAmpFadeEnabled; float gDebugViewMode;\n"
        "\tfloat gDisplacementBias; float gFactorSnapStep; float gDispDirWorldUp; float gWireframeMode;\n"
        "\tfloat gSharpMip; float gScale; float gHfWeight; float gSpikeCap;\n"
        "\tfloat gSmoothHi; float gSmoothHiMid; float gSmoothMid; float gSmoothLo;\n"
        "\tfloat gFarHi; float gFarMid; float gSkipDistance; float _gPad1;\n"
        "\tfloat4 gBlend1Mask; float4 gBlend2Mask; float4 gBlend3Mask;\n"
        "};\n"
        "float DustLum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n\n"
        "// Single-biome-layer replica: mirrors computeBiome's albedo math\n"
        "// minus the normal/absorbance branches and distance fadeout. The DS\n"
        "// uses the same formula on its mirrored textures to compute\n"
        "// displacement = visible PS luminance (no chunk seams).\n"
        "float3 DustReplicaBiomeAlbedo(Texture2DArray dmap, float3 tc, float2 cliffBlend,\n"
        "\tfloat4 weights, float4 map, float4 colour, float4 sA, float4 sB, float4 sC, float4 oMult)\n"
        "{\n"
        "\tconst float3 white = float3(1,1,1);\n"
        "\tfloat3 cB  = dmap.Sample(Anisotropic, float3(tc.xy * sB.xy, 0)).rgb * colour.rgb;\n"
        "\tfloat3 cS  = dmap.Sample(Anisotropic, float3(tc.xy * sA.xy, 1)).rgb * colour.rgb;\n"
        "\tfloat3 cG  = dmap.Sample(Anisotropic, float3(tc.xy * sB.zw, 3)).rgb * lerp(white, colour.rgb, oMult.y);\n"
        "\tfloat3 cD  = dmap.Sample(Anisotropic, float3(tc.xy * sC.xy, 4)).rgb * lerp(white, colour.rgb, oMult.z);\n"
        "\tfloat3 cR  = dmap.Sample(Anisotropic, float3(tc.xy * sC.zw, 5)).rgb * lerp(white, colour.rgb, oMult.w);\n"
        "\tfloat3 cCx = dmap.Sample(Anisotropic, float3(tc.yz * sA.zw, 2)).rgb;\n"
        "\tfloat3 cCz = dmap.Sample(Anisotropic, float3(tc.xz * sA.zw, 2)).rgb;\n"
        "\tfloat3 cC  = (cCx * cliffBlend.x + cCz * cliffBlend.y) * lerp(white, colour.rgb, oMult.x);\n"
        "\tfloat3 a = lerp(cB, cG, map.r);\n"
        "\ta = lerp(a, cS, weights.x);\n"
        "\ta = lerp(a, cD, map.b);\n"
        "\ta = lerp(a, cR, map.a);\n"
        "\ta = lerp(a, cC, weights.y);\n"
        "\treturn a;\n"
        "}\n\n";
    result.insert(pos1, inject1);

    // Override albedo at the writeAlbedo call. At this point biome.albedo.rgb
    // is the FINAL composed PS color. Mode 1 = grayscale luminance debug;
    // mode 2 = chunk-edge highlight (red ridges along chunk boundaries, or the
    // whole chunk magenta if chunk bounds couldn't be detected for this draw).
    const char* anchor3 = "writeAlbedo   ( buffer, biome.albedo.rgb, fragCoord.xy );";
    size_t pos3 = result.find(anchor3);
    if (pos3 == std::string::npos)
    {
        Log("ShaderPatch[TerrainHeightDebug]: anchor 'writeAlbedo' not found, skipping");
        return src;
    }
    std::string replacement =
        "if (gDebugViewMode > 2.5) {\n"
        "\t\t// Mode 3: radius rings. Draws a color-coded contour ring on the\n"
        "\t\t// terrain at each tess distance threshold (camera-relative, the\n"
        "\t\t// same metric the fades + CPU-skip use), over dimmed terrain so\n"
        "\t\t// each radius is visible where it lands in the world.\n"
        "\t\tfloat dustDist = length(worldPos - cameraPos);\n"
        "\t\tfloat dustL = DustLum(biome.albedo.rgb);\n"
        "\t\tfloat3 dustColor = float3(dustL, dustL, dustL) * 0.25;\n"
        "\t\t// Ring half-width grows with distance so far rings stay visible\n"
        "\t\t// (a fixed world width goes sub-pixel thin at range).\n"
        "\t\tfloat dustRingW = max(40.0, dustDist * 0.01);\n"
        "\t\t// Nearer-listed rings win on overlap (e.g. coincident fade starts).\n"
        "\t\tif (gSkipDistance > 1.0 && abs(dustDist - gSkipDistance) < dustRingW) dustColor = float3(1.0, 1.0, 1.0);\n"
        "\t\tif (abs(dustDist - gFarMid)        < dustRingW) dustColor = float3(1.0, 0.5, 0.0);\n"
        "\t\tif (abs(dustDist - gFarHi)         < dustRingW) dustColor = float3(1.0, 1.0, 0.0);\n"
        "\t\tif (abs(dustDist - gFactFadeEnd)   < dustRingW) dustColor = float3(1.0, 0.0, 1.0);\n"
        "\t\tif (abs(dustDist - gAmpFadeEnd)    < dustRingW) dustColor = float3(1.0, 0.0, 0.0);\n"
        "\t\tif (abs(dustDist - gAmpFadeStart)  < dustRingW) dustColor = float3(0.0, 1.0, 1.0);\n"
        "\t\tif (abs(dustDist - gFactFadeStart) < dustRingW) dustColor = float3(0.0, 1.0, 0.0);\n"
        "\t\tbiome.albedo.rgb = dustColor;\n"
        "\t} else if (gDebugViewMode > 1.5) {\n"
        "\t\t// Mode 2: diff overlay. Computes the same BLEND0+1+2+3 replica\n"
        "\t\t// the DS uses for displacement, displays |gtLum - replicaLum|\n"
        "\t\t// as grayscale. Black = pixel-exact (DS displacement matches PS\n"
        "\t\t// visible). Bright = where any DS seam comes from.\n"
        "\t\tfloat dust_slope = 1.0 - normalize(normal).y;\n"
        "\t\tfloat4 dust_w0 = smoothstep(slopeMin-slopeBlend, slopeMin, dust_slope) * smoothstep(slopeMax+slopeBlend, slopeMax, dust_slope);\n"
        "\t\tfloat3 dust_a = DustReplicaBiomeAlbedo(diffuseMaps, texCoords, cliffBlend, dust_w0, map, colour, scalesA, scalesB, scalesC, overlayMult);\n"
        "\t\tdust_a *= brightnessFix.x;\n"
        "\t\t#ifdef BLEND1\n"
        "\t\tfloat4 dust_bw = blendMap.Sample(Linear, mapCoords.zw);\n"
        "\t\tfloat dust_w = 1.0 - dust_bw[BLEND1];\n"
        "\t\t#ifdef BLEND2\n"
        "\t\tdust_w -= dust_bw[BLEND2];\n"
        "\t\t#ifdef BLEND3\n"
        "\t\tdust_w -= dust_bw[BLEND3];\n"
        "\t\t#endif\n"
        "\t\t#endif\n"
        "\t\tdust_a *= dust_w;\n"
        "\t\tfloat3 dust_tc1 = float3(texCoords.xy, texCoordsV.x);\n"
        "\t\tfloat4 dust_w1 = smoothstep(slopeMin1-slopeBlend1, slopeMin1, dust_slope) * smoothstep(slopeMax1+slopeBlend1, slopeMax1, dust_slope);\n"
        "\t\tfloat3 dust_a1 = DustReplicaBiomeAlbedo(diffuseMaps1, dust_tc1, cliffBlend, dust_w1, map, colour, scalesA1, scalesB1, scalesC1, overlayMult1);\n"
        "\t\tdust_a += dust_a1 * brightnessFix.y * dust_bw[BLEND1];\n"
        "\t\t#ifdef BLEND2\n"
        "\t\tfloat3 dust_tc2 = float3(texCoords.xy, texCoordsV.y);\n"
        "\t\tfloat4 dust_w2 = smoothstep(slopeMin2-slopeBlend2, slopeMin2, dust_slope) * smoothstep(slopeMax2+slopeBlend2, slopeMax2, dust_slope);\n"
        "\t\tfloat3 dust_a2 = DustReplicaBiomeAlbedo(diffuseMaps2, dust_tc2, cliffBlend, dust_w2, map, colour, scalesA2, scalesB2, scalesC2, overlayMult2);\n"
        "\t\tdust_a += dust_a2 * brightnessFix.z * dust_bw[BLEND2];\n"
        "\t\t#ifdef BLEND3\n"
        "\t\tfloat3 dust_tc3 = float3(texCoords.xy, texCoordsV.z);\n"
        "\t\tfloat4 dust_w3 = smoothstep(slopeMin3-slopeBlend3, slopeMin3, dust_slope) * smoothstep(slopeMax3+slopeBlend3, slopeMax3, dust_slope);\n"
        "\t\tfloat3 dust_a3 = DustReplicaBiomeAlbedo(diffuseMaps3, dust_tc3, cliffBlend, dust_w3, map, colour, scalesA3, scalesB3, scalesC3, overlayMult3);\n"
        "\t\tdust_a += dust_a3 * brightnessFix.w * dust_bw[BLEND3];\n"
        "\t\t#endif\n"
        "\t\t#endif\n"
        "\t\t#endif\n"
        "\t\tfloat dust_gtLum   = DustLum(biome.albedo.rgb);\n"
        "\t\tfloat dust_candLum = DustLum(dust_a);\n"
        "\t\tfloat dust_diff = saturate(abs(dust_gtLum - dust_candLum) * 5.0);\n"
        "\t\tbiome.albedo.rgb = float3(dust_diff, dust_diff, dust_diff);\n"
        "\t} else if (gDebugViewMode > 0.5) {\n"
        "\t\t// Mode 1: visible PS luminance as grayscale.\n"
        "\t\tfloat dustDbgL = DustLum(biome.albedo.rgb);\n"
        "\t\tbiome.albedo.rgb = float3(dustDbgL, dustDbgL, dustDbgL);\n"
        "\t}\n"
        "\twriteAlbedo   ( buffer, biome.albedo.rgb, fragCoord.xy );";
    result.replace(pos3, strlen(anchor3), replacement);

    Log("ShaderPatch[TerrainHeightDebug]: injected heightmap debug view into terrain main_fs");
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

    // Capture cbuffer field offsets for the volumetric/shadow-aware plugins
    // (no-op outside the CSM-variant deferred main_fs).
    CSMCapture::DiscoverOffsets(reflector);

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
    ZoneScopedN("HookedD3DCompile");
    if (D3D11Hook::IsShutdownSignaled())
        return oD3DCompile(pSrcData, SrcDataSize, pSourceName,
                            pDefines, pInclude, pEntrypoint, pTarget,
                            Flags1, Flags2, ppCode, ppErrorMsgs);

    // [Dust] Terrain VS upgrade: Kenshi compiles terrain.hlsl main_vs as
    // vs_4_0, which cannot be paired with HS/DS (D3D silently drops geometry
    // with mismatched stage versions). Upgrade to vs_5_0 so we can route
    // these draws through tessellation. Detection: entry "main_vs" + source
    // contains terrain-uniform markers.
    if (pEntrypoint && pTarget && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_vs") == 0 &&
        strncmp(pTarget, "vs_4_", 5) == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        bool isTerrainVs = src.find("biomeData") != std::string::npos &&
                           src.find("overlayData") != std::string::npos &&
                           src.find("morph") != std::string::npos;
        if (isTerrainVs)
        {
            // [Dust] Add new VS output `oWvpCol1` carrying column 1 of the
            // worldViewProjMatrix (the Y-axis projection vector). The DS
            // uses it for sub-vertex Y displacement: clip += h * wvp_col1.
            // No matrix mirror to DS needed — VS has the matrix natively.
            //
            // Two injection points:
            //   1. Output param after `oTexV : TEXCOORD5,` (TEXTURED block).
            //   2. Assignment after `oPosition = mul(worldViewProjMatrix,...);`.
            const std::string outAnchor = "out float4 oTexV        : TEXCOORD5,";
            size_t outPos = src.find(outAnchor);
            if (outPos != std::string::npos)
            {
                const std::string outInjection =
                    outAnchor +
                    std::string("\n\tout float4 oWvpCol1     : TEXCOORD6,  // [Dust] WVP column 1 (Y)"
                                "\n\tout float4 oWvpCol0     : TEXCOORD7,  // [Dust] WVP column 0 (X)"
                                "\n\tout float4 oWvpCol2     : TEXCOORD8,  // [Dust] WVP column 2 (Z)");
                src.replace(outPos, outAnchor.size(), outInjection);
            }
            const std::string asgnAnchor = "oPosition = mul(worldViewProjMatrix, position);";
            size_t asgnPos = src.find(asgnAnchor);
            if (asgnPos != std::string::npos && outPos != std::string::npos)
            {
                const std::string asgnInjection = asgnAnchor + std::string(
                    "\n#ifdef TEXTURED\n"
                    "\toWvpCol1 = float4(worldViewProjMatrix._12, worldViewProjMatrix._22,"
                    " worldViewProjMatrix._32, worldViewProjMatrix._42);\n"
                    "\toWvpCol0 = float4(worldViewProjMatrix._11, worldViewProjMatrix._21,"
                    " worldViewProjMatrix._31, worldViewProjMatrix._41);\n"
                    "\toWvpCol2 = float4(worldViewProjMatrix._13, worldViewProjMatrix._23,"
                    " worldViewProjMatrix._33, worldViewProjMatrix._43);\n"
                    "#endif");
                src.replace(asgnPos, asgnAnchor.size(), asgnInjection);
                Log("ShaderPatch: injected oWvpCol1 output into terrain main_vs");
            }
            else
            {
                Log("ShaderPatch: terrain VS oWvpCol1 anchors not all found");
            }
            HRESULT hr = oD3DCompile(src.c_str(), src.size(), pSourceName,
                                      pDefines, pInclude, pEntrypoint, "vs_5_0",
                                      Flags1, Flags2, ppCode, ppErrorMsgs);
            if (SUCCEEDED(hr))
            {
                Log("ShaderPatch: upgraded terrain main_vs %s → vs_5_0", pTarget);
                if (ppCode && *ppCode)
                {
                    // Log every upgraded terrain VS signature so we can see both
                    // TEXTURED and non-TEXTURED variants and compare to HS input.
                    static int sLogged = 0;
                    sLogged++;
                    char tag[32];
                    snprintf(tag, sizeof(tag), "VS_terrain#%d", sLogged);
                    TerrainTess::LogShaderSignature(
                        (*ppCode)->GetBufferPointer(),
                        (*ppCode)->GetBufferSize(), tag);
                    SurveyRecorder::OnShaderCompiled(pSrcData, SrcDataSize,
                        pEntrypoint, "vs_5_0", pSourceName,
                        (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                }
                return hr;
            }
            Log("ShaderPatch: terrain VS upgrade failed (0x%08X), falling back", hr);
            if (ppErrorMsgs && *ppErrorMsgs) {
                Log("ShaderPatch: error: %s", (const char*)(*ppErrorMsgs)->GetBufferPointer());
                (*ppErrorMsgs)->Release();
                *ppErrorMsgs = nullptr;
            }
        }
    }

    // Local helper: capture BLEND1/2/3 #defines for any terrain PS variant.
    // Must be called for EVERY successful main_fs / mapfeature_fs compile —
    // including the terrain-debug-patched path below — so the DS knows which
    // blendMap channel weights each layer at draw time.
    auto captureBlendDefines = [&](const void* code, size_t codeSize) {
        if (!pEntrypoint || !code || codeSize == 0) return;
        if (strcmp(pEntrypoint, "main_fs") != 0 &&
            strcmp(pEntrypoint, "mapfeature_fs") != 0) return;
        int b1 = -1, b2 = -1, b3 = -1;
        if (pDefines)
        {
            for (const D3D_SHADER_MACRO* m = pDefines; m->Name; m++)
            {
                if (!m->Definition) continue;
                if      (strcmp(m->Name, "BLEND1") == 0) b1 = atoi(m->Definition);
                else if (strcmp(m->Name, "BLEND2") == 0) b2 = atoi(m->Definition);
                else if (strcmp(m->Name, "BLEND3") == 0) b3 = atoi(m->Definition);
            }
        }
        TerrainTess::OnTerrainPsCompiled(code, codeSize, b1, b2, b3);
    };

    // [Dust] Terrain main_fs heightmap-debug-view patch. Detects by markers
    // unique to terrain (computeBiome, scalesA), distinguishing from deferred
    // main_fs. Falls through to compile original on patcher/compile failure.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_fs") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        bool isTerrainFs = src.find("computeBiome") != std::string::npos &&
                           src.find("scalesA") != std::string::npos &&
                           src.find("DustReplicaBiomeAlbedo") == std::string::npos;
        if (isTerrainFs)
        {
            std::string patched = PatchTerrainShaderForHeightDebug(src);
            if (patched.size() != src.size())
            {
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, pTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    if (ppCode && *ppCode)
                    {
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName,
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                        captureBlendDefines((*ppCode)->GetBufferPointer(),
                                            (*ppCode)->GetBufferSize());
                    }
                    return hr;
                }
                Log("ShaderPatch[TerrainHeightDebug]: compile failed, falling back to original");
                if (ppErrorMsgs && *ppErrorMsgs)
                {
                    Log("ShaderPatch[TerrainHeightDebug]: error: %s",
                        (const char*)(*ppErrorMsgs)->GetBufferPointer());
                    (*ppErrorMsgs)->Release();
                    *ppErrorMsgs = nullptr;
                }
            }
        }
    }

    // [Dust] Kenshi compiles the deferred main_fs/light_fs as ps_4_0, which predates
    // TextureCubeArray (needed by the point-light shadow injection). Bump 4_0 -> 4_1
    // (FL11 hardware, backward-compatible) so the cube-array sample compiles. Used for
    // both deferred compile calls below.
    const char* defTarget = (pTarget && strncmp(pTarget, "ps_4_0", 6) == 0) ? "ps_4_1" : pTarget;

    // Detect the deferred lighting pixel shader: entry point is "main_fs"
    // and source contains deferred-specific identifiers.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_fs") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        if (src.find("CalcEnvironmentLight") != std::string::npos &&
            src.find("dustAoTex") == std::string::npos)  // not already patched
        {
            std::string patched = PatchDeferredShader(src);
            if (patched.size() != src.size())
            {
                Log("ShaderPatch: patched deferred main_fs (%zu -> %zu bytes)",
                    src.size(), patched.size());
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, defTarget,
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
                    const char* msg = (const char*)(*ppErrorMsgs)->GetBufferPointer();
                    Log("ShaderPatch: error: %s", msg);
                    snprintf(gCompileError, sizeof(gCompileError), "deferred main_fs: %s", msg);
                    (*ppErrorMsgs)->Release();
                    *ppErrorMsgs = nullptr;
                }
                {
                    std::string dumpPath = DustLogDir() + "logs\\patched_deferred_fail.hlsl";
                    FILE* df = nullptr;
                    fopen_s(&df, dumpPath.c_str(), "w");
                    if (df) { fwrite(patched.c_str(), 1, patched.size(), df); fclose(df); }
                    Log("ShaderPatch: dumped patched source to %s", dumpPath.c_str());
                }
                // Fall through to compile original below
            }
        }
    }

    // Detect deferred light_fs (point lights / spotlights): same source as main_fs,
    // different entry point. Patch identically so the global AO declarations and
    // the light_fs injection are present when the compiler processes this entry point.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "light_fs") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        if (src.find("CalcEnvironmentLight") != std::string::npos &&
            src.find("dustAoTex") == std::string::npos)
        {
            std::string patched = PatchDeferredShader(src);
            if (patched.size() != src.size())
            {
                Log("ShaderPatch: patched deferred light_fs (%zu -> %zu bytes)",
                    src.size(), patched.size());
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, defTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    if (ppCode && *ppCode)
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName,
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                    return hr;
                }

                Log("ShaderPatch: patched light_fs failed to compile, falling back to original");
                if (ppErrorMsgs && *ppErrorMsgs)
                {
                    const char* msg = (const char*)(*ppErrorMsgs)->GetBufferPointer();
                    Log("ShaderPatch: error: %s", msg);
                    snprintf(gCompileError, sizeof(gCompileError), "deferred light_fs: %s", msg);
                    (*ppErrorMsgs)->Release();
                    *ppErrorMsgs = nullptr;
                }
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
                    const char* msg = (const char*)(*ppErrorMsgs)->GetBufferPointer();
                    Log("ShaderPatch: error: %s", msg);
                    snprintf(gCompileError, sizeof(gCompileError), "objects shader: %s", msg);
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

        captureBlendDefines((*ppCode)->GetBufferPointer(),
                            (*ppCode)->GetBufferSize());
    }

    return hr;
}

} // namespace ShaderPatch
