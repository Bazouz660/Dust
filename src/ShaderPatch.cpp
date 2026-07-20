#include "ShaderPatch.h"
#include "DustLog.h"
#include "SurveyRecorder.h"
#include "D3D11Hook.h"

#include <string>
#include <cstring>
#include <set>
#include <mutex>

namespace ShaderPatch
{

PFN_D3DCompileHook oD3DCompile = nullptr;

// Diagnostic: when [Debug] DumpInjectedShaders=1 in Dust.ini, write the ORIGINAL and INJECTED HLSL of
// every GBuffer shader Dust rewrites to DustLogDir()\injected_shaders\, one pair per distinct variant.
// Lets an injection bug on a specific game/mod shader variant be inspected offline (the pipeline survey
// only captures a few startup frames, so it misses on-demand renders like item-icon generation). Off by
// default — zero cost when the flag is absent.
static bool DumpInjectedEnabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = GetPrivateProfileIntA("Debug", "DumpInjectedShaders", 0,
                                       (DustLogDir() + "Dust.ini").c_str()) ? 1 : 0;
    return cached != 0;
}

// Serializes the de-dup set in DumpInjection — HookedD3DCompile (D3DCompile) is free-threaded.
static std::mutex sDumpSeenMutex;

static void DumpInjection(const char* tag, const char* srcName, const char* entry,
                          const std::string& original, const std::string& patched,
                          const D3D_SHADER_MACRO* pDefines = nullptr)
{
    if (!DumpInjectedEnabled()) return;

    // One dump per (source, entry, variant): the variant hash separates #define permutations of the
    // same file, and de-dups the thousands of identical recompiles across draws.
    std::string base = std::string(srcName ? srcName : "unknown") + "_" + (entry ? entry : "x");
    for (char& ch : base)
        if (strchr("\\/:*?\"<>| ", ch)) ch = '_';
    char hash[16];
    snprintf(hash, sizeof(hash), "_%08zX", std::hash<std::string>{}(original) & 0xFFFFFFFF);
    base += hash;

    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(sDumpSeenMutex);
        if (!seen.insert(base).second) return;
    }

    // Header records the permutation's defines — the variant hash alone doesn't say WHICH #define
    // set a dump is, and GBUFFER/backwards-compat defines drive the injection choices.
    std::string hdr;
    if (pDefines)
    {
        hdr = "// [Dust] DEFINES:";
        for (const D3D_SHADER_MACRO* m = pDefines; m && m->Name; ++m)
        {
            hdr += ' ';
            hdr += m->Name;
            if (m->Definition && m->Definition[0]) { hdr += '='; hdr += m->Definition; }
        }
        hdr += "\n";
    }

    std::string dir = DustLogDir() + "injected_shaders";
    CreateDirectoryA(dir.c_str(), nullptr);
    for (int i = 0; i < 2; ++i)
    {
        std::string path = dir + "\\" + tag + "_" + base + (i == 0 ? ".orig.hlsl" : ".injected.hlsl");
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) continue;
        const std::string& s = (i == 0) ? original : patched;
        if (!hdr.empty()) fwrite(hdr.data(), 1, hdr.size(), f);
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
    Log("ShaderPatch: dumped injection %s / %s -> injected_shaders\\%s", srcName ? srcName : "?",
        entry ? entry : "?", base.c_str());
}

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
    // and vanilla computeShadowMultiplier (CSM) with improved filtering: Poisson disk,
    // per-pixel rotation, PCSS penumbra, cascade blending. Parameters come from a
    // constant buffer (b7) bound by the Shadows effect plugin.

    // Injection 3: Add cbuffer declaration + DustRTWShadow + DustCascadeShadow functions.
    // Insert before main_vs so they're defined after includes (GetOffsetLocationS,
    // pcfSample, computeShadowMultiplier) but before use in main_fs. shadowFunctions.hlsl
    // defines SHADOW_MAP_COUNT (default 4), pcfSample and computeShadowMultiplier
    // unconditionally, so the CSM helpers compile in RTW-mode variants too.
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
            "\tfloat dustRtwCliffFixEnabled;\n"
            "\tfloat dustRtwCliffFixDistance;\n"
            "\tfloat dustCsmFilterRadius;\n"
            "\tfloat dustCsmBlendEnabled;\n"
            "\tfloat dustCsmBlendWidth;\n"
            "\tfloat dustRtwQuality;\n"
            "\tfloat dustShadowTexel;\n"
            "\tfloat dustCsmFarSoftness;\n"
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
            "\tfloat4 sc = mul(shadowMatrix, float4(worldPos, 1));\n"
            "\tfloat2 center = DustGetOffsetLocationS(wMap, sc.xy);\n"
            "\tfloat2 edge = saturate(abs(center - 0.5) * 20 - 9);\n"
            "\tb += edgeBias * (edge.x + edge.y);\n"
            "\tfloat sd = saturate(mul(shadowMatrix, float4(worldPos, 1)).z);\n"
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
            // The incoming bias b is the vanilla RTW shadow_bias (0.00003,
            // rtwshadows.program) plus the edge-fade term above; no extra
            // depth/normal/slope bias is applied.
            "\tfloat fr = dustRtwFilterRadius;\n"
            "\tfloat ls = dustRtwLightSize;\n"
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

            "float2 DustVogel(int i, int count, float rotOffset) {\n"
            "\tconst float kGolden = 2.399963229728653;\n"
            "\tfloat r = sqrt((i + 0.5) / (float)count);\n"
            "\tfloat theta = i * kGolden + rotOffset;\n"
            "\tfloat s, c;\n"
            "\tsincos(theta, s, c);\n"
            "\treturn float2(c, s) * r;\n"
            "}\n\n"

            "float DustPcfBilinear(sampler2D sm, float3 shadowUv, float3x3 sampleProj,\n"
            "                      float2 offset2D, float texel) {\n"
            "\tfloat3 s = shadowUv + mul(sampleProj, float3(offset2D, 0));\n"
            "\tfloat invT = 1.0 / texel;\n"
            "\tfloat2 tc = s.xy * invT - 0.5;\n"
            "\tfloat2 f = frac(tc);\n"
            "\tfloat2 uv0 = (floor(tc) + 0.5) * texel;\n"
            "\tfloat d00 = tex2Dlod(sm, float4(uv0,                          0, 0)).x;\n"
            "\tfloat d10 = tex2Dlod(sm, float4(uv0 + float2(texel, 0),       0, 0)).x;\n"
            "\tfloat d01 = tex2Dlod(sm, float4(uv0 + float2(0, texel),       0, 0)).x;\n"
            "\tfloat d11 = tex2Dlod(sm, float4(uv0 + float2(texel, texel),   0, 0)).x;\n"
            "\tfloat4 lit = step(s.zzzz, float4(d00, d10, d01, d11));\n"
            "\tfloat bottom = lerp(lit.x, lit.y, f.x);\n"
            "\tfloat top    = lerp(lit.z, lit.w, f.x);\n"
            "\treturn lerp(bottom, top, f.y);\n"
            "}\n\n"

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
            "\tfloat ang = noise.x * 6.28318530718;\n"
            "\n"
            // Fully-lit early-out: 4 rotated taps at the filter radius. If nothing
            // is occluded within reach of the filter, the pixel is lit — skip the
            // full 16-tap bilinear filter (most pixels in a sunlit scene).
            "\tfloat2x2 filterScale = float2x2(baseRadius, 0, 0, baseRadius);\n"
            "\tfloat occ = 0;\n"
            "\t[unroll]\n"
            "\tfor (int j = 0; j < 4; j++)\n"
            "\t\tif (pcfSample(shadowDepthMap, shadowUv, sampleProj, filterScale, DustVogel(j, 16, ang)) < 0) occ += 1;\n"
            "\tif (occ <= 0) return 1.0;\n"
            "\n"
            "\tfloat shadow = 0;\n"
            "\t[unroll]\n"
            "\tfor (int k = 0; k < 16; k++) {\n"
            "\t\tfloat2 off = DustVogel(k, 16, ang) * baseRadius;\n"
            "\t\tshadow += DustPcfBilinear(shadowDepthMap, shadowUv, sampleProj, off, dustShadowTexel);\n"
            "\t}\n"
            "\treturn shadow * (1.0 / 16.0);\n"
            "}\n\n"

            "float DustSampleCascade4(\n"
            "\tsampler2D shadowDepthMap, sampler2D jitterMap,\n"
            "\tfloat3 shadowUv, float3x3 sampleProj, float baseRadius)\n"
            "{\n"
            "\tfloat2 noise = tex2Dlod(jitterMap, float4(shadowUv.xy * 1024.0, 0, 0)).xy;\n"
            "\tfloat ang = noise.x * 6.28318530718;\n"
            "\n"
            "\tfloat2x2 filterScale = float2x2(baseRadius, 0, 0, baseRadius);\n"
            "\tfloat occ = 0;\n"
            "\t[unroll]\n"
            "\tfor (int j = 0; j < 4; j++)\n"
            "\t\tif (pcfSample(shadowDepthMap, shadowUv, sampleProj, filterScale, DustVogel(j, 8, ang)) < 0) occ += 1;\n"
            "\tif (occ <= 0) return 1.0;\n"
            "\n"
            "\tfloat shadow = 0;\n"
            "\t[unroll]\n"
            "\tfor (int k = 0; k < 8; k++) {\n"
            "\t\tfloat2 off = DustVogel(k, 8, ang) * baseRadius;\n"
            "\t\tshadow += DustPcfBilinear(shadowDepthMap, shadowUv, sampleProj, off, dustShadowTexel);\n"
            "\t}\n"
            "\treturn shadow * (1.0 / 8.0);\n"
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
            // 0.75 matches vanilla's effective footprint: vanilla scales its
            // hex offsets (avg magnitude ~2.07) by csmParams[i][1]*0.3; our
            // Poisson offsets (avg ~0.82) need ~0.75 to land at the same
            // average filter width. dustCsmFilterRadius (default 1.0) is the
            // user multiplier on top.
            "\tfloat baseRadius0 = csmParams[idx][1] * dustCsmFilterRadius * 0.75;\n"
            "\tif (idx >= SHADOW_MAP_COUNT - 1) baseRadius0 *= dustCsmFarSoftness;\n"
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
            "\t\t\tfloat baseRadius1 = csmParams[idx + 1][1] * dustCsmFilterRadius * 0.75;\n"
            "\t\t\tif (idx + 1 >= SHADOW_MAP_COUNT - 1) baseRadius1 *= dustCsmFarSoftness;\n"
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
        Log("ShaderPatch: injected DustShadowParams cbuffer + DustRTWShadow + DustCascadeShadow");
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

    // CSM redirect: replace `shadow = computeShadowMultiplier(` with
    // `shadow = DustCascadeShadow(`. The argument list is identical, so this
    // is a pure name swap; DustCascadeShadow falls back to the vanilla
    // function internally when dustShadowEnabled < 0.5 (no ternary — the
    // out debugColorMask param can't live inside one).
    const char* csmCallAnchor = "shadow = computeShadowMultiplier(";
    size_t csmAnchorPos = result.find(csmCallAnchor);
    if (csmAnchorPos != std::string::npos)
    {
        const char* csmReplacement = "shadow = DustCascadeShadow(";
        result.replace(csmAnchorPos, strlen(csmCallAnchor), csmReplacement);
        Log("ShaderPatch: redirected computeShadowMultiplier -> DustCascadeShadow");
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

// Patch the point/spot light-volume pixel shader (light_fs in deferred.hlsl) to apply
// the same direct-light AO term the sun pass uses. Without this, SSAO's "Direct Light AO"
// only darkens the sun; point and spot lights (rendered as additive forward light volumes
// through light_fs, a separate compile) stay fully lit regardless of the setting.
// The AO map is fed to s8/s9 per-draw by the host's light-volume path (see D3D11Hook.cpp),
// which recognizes this shader via the bytecode registered in HookedD3DCompile below.
// Returns the modified source, or the original if anchors weren't found.
static std::string PatchLightVolumeShader(const std::string& src)
{
    std::string result = src;

    // Injection 1: declare the AO texture/sampler as globals with EXPLICIT texture
    // registers t8/t9. This is load-bearing: light_fs only uses t0-t2, so a combined
    // "sampler aoMap : register(s8)" binds the SAMPLER to s8 but lets the compiler
    // auto-assign the TEXTURE to t3 — while the host binds the AO SRV to t8. light_fs
    // would then read whatever the engine left at t3 (a tiled texture => screen-space
    // rectangles). Separate Texture2D : register(t8) pins the texture to t8 to match the
    // host bind (D3D11Hook.cpp LightVolumeAoScope). Verified with fxc.
    // Anchor: "float4 light_fs(" — the function definition (unique in deferred.hlsl).
    const char* anchorDecl = "float4 light_fs(";
    size_t posDecl = result.find(anchorDecl);
    if (posDecl == std::string::npos)
    {
        Log("ShaderPatch: light_fs anchor 'float4 light_fs(' not found, skipping");
        return src;
    }
    result.insert(posDecl,
        "// [Dust] Direct-light AO (host binds per light-volume draw): SSAO map at t8/t9,\n"
        "// RTGI AO at t10 (white when RTGI is off). Explicit registers are REQUIRED here\n"
        "// (light_fs only uses t0-t2, so the compiler would otherwise pack these low).\n"
        "Texture2D    dustLvAoTex       : register(t8);\n"
        "Texture2D    dustLvAoParamsTex : register(t9);\n"
        "Texture2D    dustLvRtgiAoTex   : register(t10);\n"
        "SamplerState dustLvAoSamp      : register(s8);\n\n");

    // Injection 2: fade the light's contribution by the direct-light AO amount.
    // Anchor: "color = color * attenuation * power;" — unique to light_fs. texCoord is
    // already in scope (computed from VPOS at the top of light_fs). Same directFade
    // formula as the sun pass: lerp(1, ao, directAO).
    const char* anchorApply = "color = color * attenuation * power;";
    size_t posApply = result.find(anchorApply);
    if (posApply == std::string::npos)
    {
        Log("ShaderPatch: light_fs anchor 'attenuation * power' not found, skipping");
        return src;
    }
    posApply += strlen(anchorApply);
    result.insert(posApply,
        "\n\t// [Dust] Direct-light AO for point/spot lights\n"
        "\tfloat aoLV = dustLvAoTex.Sample(dustLvAoSamp, texCoord).r;\n"
        "\tfloat directAOLV = dustLvAoParamsTex.Sample(dustLvAoSamp, texCoord).r;\n"
        "\tcolor *= lerp(1.0, aoLV, directAOLV);\n"
        "\tcolor *= dustLvRtgiAoTex.Sample(dustLvAoSamp, texCoord).r;  // RTGI AO (white when off)");

    return result;
}

// ---- Per-object motion vectors: inject a velocity output into the game's own GBuffer shaders ----
// The robust replacement for the geometry-replay pass. objects.hlsl main_vs carries current +
// previous clip position (prev = camera reprojection of cur, from a b13 CB); main_ps writes the
// screen-space velocity to SV_Target3 (the velocity RT appended to the GBuffer). Because it runs in
// the real GBuffer pass, coverage/alpha/animation are all exact — no depth-matching.
static bool Has(const std::string& s, const char* a) { return s.find(a) != std::string::npos; }

// Insert `text` after the first occurrence of `anchor` AT OR AFTER `from` (the .hlsl file contains
// BOTH the VS and PS, so every anchor search must be scoped to the target entry function). Advances
// `from` to just past the insertion so subsequent inserts stay ordered. Returns false on miss.
static bool InsAfter(std::string& s, size_t& from, const char* anchor, const std::string& text)
{
    size_t p = s.find(anchor, from);
    if (p == std::string::npos) return false;
    p += strlen(anchor);
    s.insert(p, text);
    from = p + text.size();
    return true;
}
static bool InsBefore(std::string& s, size_t& from, const char* anchor, const std::string& text)
{
    size_t p = s.find(anchor, from);
    if (p == std::string::npos) return false;
    s.insert(p, text);
    from = p + text.size();
    return true;
}

// Insert `text` at the END of the entry function's parameter list (just before its closing paren).
//
// THIS PLACEMENT IS LOAD-BEARING. Declaring the injected interpolants LAST gives them the HIGHEST
// signature registers, so no existing parameter's register can shift. This pipeline routes VS->PS
// varyings BY REGISTER (live-proven 2026-07-20 — see InjPS), so the register layout IS the
// contract: any uninjected consumer (rtticons.hlsl in the icon pipeline, construction, blood) must
// see every vanilla output at its vanilla register. Mid-list insertion shifted them (COLOR0
// reg7 -> reg9 with oDustCur landing on reg7 = clip position as the item's dye colour — the
// original quadrant-icon bug). Appending keeps every vanilla register intact, and the injected
// partner PSes append the SAME sequence so their iDustCur/iDustPrev registers line up.
static bool InsAtParamEnd(std::string& s, const char* fnAnchor, const std::string& text)
{
    size_t fn = s.find(fnAnchor);
    if (fn == std::string::npos) return false;
    size_t open = s.find('(', fn);
    if (open == std::string::npos) return false;
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) { s.insert(i, text); return true; }
    }
    return false;
}

// Sacrificial COLOR0 pad for the VSes whose meshes the item-icon pipeline renders (objects,
// severed_limb — per the ICONDRAW ground-truth probe, icons draw with objects.hlsl main_vs +
// uninjected rtticons.hlsl main_ps). The rtticons COLOURING variant reads `colour : COLOR0` at
// input reg 7; the plain objects VS has no COLOR0 output, and this pipeline routes varyings BY
// REGISTER, so without the pad that input would receive the first appended Dust output (clip
// position) instead of vanilla's nothing/zeros. Declaring COLOR0 = 0 as the FIRST appended output
// keeps reg 7 harmless. The guard skips variants that already output a real COLOR0 (objects
// COLOURING/DUAL_TEXTURE); it is inert for sources without those defines. NOTE: the pad must be
// mirrored in every INJECTED partner PS (pad flag in kPSpecs) or the register shift breaks their
// iDustCur/iDustPrev reads — that asymmetry was the 2026-07-20 black-MV regression. (The pad did
// NOT cure the quadrant icons on its own — that corruption enters through something the icon-pair
// disassembly dump is still chasing — but it is kept as cheap register hygiene for reg 7.)
static const char* kDustPadDecl =
    "\n#if !defined(COLOURING) && !defined(DUAL_TEXTURE)"
    "\n\tout float4 oDustPad : COLOR0,"
    "\n#endif";
static const char* kDustPadAssign =
    "\n#if !defined(COLOURING) && !defined(DUAL_TEXTURE)"
    "\n\toDustPad = float4(0,0,0,0);"
    "\n#endif";

// Generic velocity injection into a GBuffer VS. fnAnchor = entry-fn start; clipAssign = the clip-output
// assignment; clipRHS = its RHS (recomputed into oDustCur — never read the SV_Position output).
// The interpolants are APPENDED to the parameter list (see InsAtParamEnd) so they never shift an
// existing parameter's register; `interp` is no longer used for placement. pad => prepend the
// sacrificial COLOR0 (see kDustPadDecl) for icon-renderable meshes.
static std::string InjVS(const std::string& src, const char* fnAnchor, const char* /*interp*/,
                         const char* clipAssign, const char* clipRHS, bool pad = false)
{
    if (Has(src, "oDustPrev")) return src;
    std::string s = src;
    size_t fn = s.find(fnAnchor);
    if (fn == std::string::npos) return src;
    // column_major is explicit: the reproj CB is uploaded column-major, but shaders differ in default
    // matrix packing (objects.hlsl column, skin.hlsl row) — without this, row-major shaders read it
    // transposed and produce garbage MVs.
    s.insert(fn, "cbuffer DustMVCB : register(b13) { column_major float4x4 dust_reproj; };\n");
    std::string decl = std::string(",") + (pad ? kDustPadDecl : "") +
                       "\n\tout float4 oDustCur : TEXCOORD12,\n\tout float4 oDustPrev : TEXCOORD13\n";
    if (!InsAtParamEnd(s, fnAnchor, decl)) return src;
    size_t from = s.find(fnAnchor);                    // scope the body search to the entry function
    if (from == std::string::npos) return src;
    std::string asgn = std::string(pad ? kDustPadAssign : "") +
                       "\n\toDustCur = " + clipRHS + "; oDustPrev = mul(dust_reproj, oDustCur);";
    if (!InsAfter(s, from, clipAssign, asgn)) return src;
    return s;
}

// Velocity injection for RIGID objects that can MOVE independently of the camera (objects.hlsl:
// weapons/tools attached to animated character bones, doors, moving props). The generic InjVS uses
// camera-only reproj (dust_reproj), which is correct only if the object's WORLD transform didn't
// change -> a swinging weapon ghosts. Here oDustPrev is the vertex reprojected by the object's OWN
// PREVIOUS world-view-proj (dust_prevWVP, bound per-draw from a spatial cross-frame match in
// MotionVectors). posExpr is the vertex position fed to the clip transform. When no match was bound
// (all-zero matrix: new object / first frame / churn) we fall back to camera reproj, exactly like the
// skinned path — so this NEVER regresses static objects (whose prevWVP equals the camera reproj anyway).
static std::string InjRigidVS(const std::string& src, const char* fnAnchor, const char* /*interp*/,
                              const char* clipAssign, const char* clipRHS, const char* posExpr)
{
    if (Has(src, "oDustPrev")) return src;
    std::string s = src;
    size_t fn = s.find(fnAnchor);
    if (fn == std::string::npos) return src;
    s.insert(fn, "cbuffer DustMVCB : register(b13) { column_major float4x4 dust_reproj; };\n"
                 "cbuffer DustPrevWVPCB : register(b12) { column_major float4x4 dust_prevWVP; };\n");
    if (!InsAtParamEnd(s, fnAnchor, ",\n\tout float4 oDustCur : TEXCOORD12,\n\tout float4 oDustPrev : TEXCOORD13\n")) return src;
    size_t from = s.find(fnAnchor);
    if (from == std::string::npos) return src;
    std::string asgn = std::string("\n\toDustCur = ") + clipRHS + ";"
        "\n\toDustPrev = mul(dust_prevWVP, " + posExpr + ");"
        // No per-object prev bound this draw => dust_prevWVP all-zero (its perspective row is 0) =>
        // fall back to camera reproj (correct for a still object, ~right for a moving one that missed).
        "\n\tif (dot(dust_prevWVP._m30_m31_m32_m33, dust_prevWVP._m30_m31_m32_m33) == 0.0) oDustPrev = mul(dust_reproj, oDustCur);";
    if (!InsAfter(s, from, clipAssign, asgn)) return src;
    return s;
}

// Insert the Dust PS inputs at the end of the entry function's parameter list, EXCEPT when the list
// carries a system-generated value input (SV_IsFrontFace in objects.hlsl's DOUBLESIDED variants):
// fxc rejects non-SV signature inputs declared after an SGV (X4576), which made the injected compile
// FAIL for every double-sided variant — the original-source fallback then silently shipped a MV-less
// PS (and dropped the alpha-dither fix with it). In that case the inputs go right BEFORE the SGV
// parameter — above its wrapping #ifdef when present, so variants without the define keep the exact
// same appended layout as before. The SGV is rasterizer-fed, so shifting ITS register is harmless,
// and everything declared before it keeps its register either way.
// pad => the paired VS carries the sacrificial COLOR0 pad (kDustPadDecl), and since this pipeline
// routes varyings BY REGISTER the PS must declare the SAME sequence: a (unused) COLOR0 input under
// the same preprocessor guard, immediately before iDustCur. Unused inputs keep their ISGN register,
// so alignment holds. Without this symmetry the PS reads the pad as iDustCur (0/0 = NaN velocity —
// the 2026-07-20 "characters and trees black in the MV view" regression).
static bool InsDustPSInputs(std::string& s, const char* fnAnchor, bool pad)
{
    size_t fn = s.find(fnAnchor);
    if (fn == std::string::npos) return false;
    size_t open = s.find('(', fn);
    if (open == std::string::npos) return false;
    size_t close = std::string::npos;
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) return false;

    const char* padBlock = pad ?
        "#if !defined(COLOURING) && !defined(DUAL_TEXTURE)\n\tfloat4 iDustPad : COLOR0,\n#endif\n\t" : "";

    size_t sgv = s.find("SV_IsFrontFace", open);
    if (sgv == std::string::npos || sgv > close)
    {
        // No SGV input — plain append, byte-identical to the previous behaviour (plus the pad).
        s.insert(close, std::string(",\n") + padBlock +
                        "float4 iDustCur : TEXCOORD12,\n\tfloat4 iDustPrev : TEXCOORD13,\n\tout float2 oDustVel : SV_Target3\n");
        return true;
    }
    size_t ins = s.rfind('\n', sgv);
    ins = (ins == std::string::npos) ? open + 1 : ins + 1;
    if (ins > open + 2)
    {
        size_t prevStart = s.rfind('\n', ins - 2);
        prevStart = (prevStart == std::string::npos) ? open + 1 : prevStart + 1;
        size_t firstNonWs = s.find_first_not_of(" \t", prevStart);
        if (firstNonWs != std::string::npos && s.compare(firstNonWs, 3, "#if") == 0)
            ins = prevStart;                        // hop above the #ifdef guarding the SGV
    }
    s.insert(ins, std::string(padBlock) +
                  "float4 iDustCur : TEXCOORD12,\n\tfloat4 iDustPrev : TEXCOORD13,\n\tout float2 oDustVel : SV_Target3,\n\t");
    return true;
}

// Insert `text` right after the entry function's opening '{'.
static bool InsAtFuncStart(std::string& s, const char* fnAnchor, const std::string& text)
{
    size_t fn = s.find(fnAnchor);
    if (fn == std::string::npos) return false;
    size_t open = s.find('(', fn);
    if (open == std::string::npos) return false;
    size_t close = std::string::npos;
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) return false;
    size_t brace = s.find('{', close);
    if (brace == std::string::npos) return false;
    s.insert(brace + 1, text);
    return true;
}

// Generic velocity injection into a GBuffer PS. GROUND TRUTH (LIVE-proven 2026-07-20): this
// pipeline routes VS->PS varyings BY REGISTER — semantic names are documentation only. (Proof: a
// VS-side COLOR0 pad without a matching PS-side input shifted every later varying and turned all
// skin-family and plain-objects MVs into NaN — black characters/trees in the MV view.) So each
// PS's injected parameter sequence must mirror its paired VS's REGISTER layout exactly:
//  - interp == nullptr (objects-style): the PS's vanilla inputs mirror the VS's vanilla outputs
//    1:1, so APPEND at the end (InsDustPSInputs), with the pad flag matching the VS spec's.
//  - interp != nullptr (skin/character/creature family): those PSes have DIFFERENT vanilla input
//    counts than skin.hlsl main_vs (hair_fs has no wet/blood inputs), so end-append cannot align
//    every family member. ANCHOR placement right after the shared TC0-5 prefix ("TEXCOORD5,")
//    on BOTH the VS and every family PS puts iDustCur/iDustPrev at v7/v8 <- o7/o8 for the whole
//    family regardless of each PS's tail (in-game verified: the 4ed6cf0 hair fix).
// gbufOut is still required as proof this really is a GBuffer PS.
static std::string InjPS(const std::string& src, const char* fnAnchor, const char* interp,
                         const char* gbufOut, const char* body, bool pad)
{
    if (Has(src, "oDustVel")) return src;
    std::string s = src;
    size_t from = s.find(fnAnchor);
    if (from == std::string::npos) return src;
    if (!Has(s, gbufOut)) return src;               // not a GBuffer PS — leave it alone
    if (interp)
    {
        if (!InsAfter(s, from, interp, "\n\tfloat4 iDustCur : TEXCOORD12,\n\tfloat4 iDustPrev : TEXCOORD13,")) return src;
        if (!InsBefore(s, from, gbufOut, "out float2 oDustVel : SV_Target3,\n\t")) return src;
    }
    else if (!InsDustPSInputs(s, fnAnchor, pad)) return src;
    from = s.find(fnAnchor);
    if (from == std::string::npos) return src;
    const std::string vel = "\n\toDustVel = (iDustCur.xy/iDustCur.w - iDustPrev.xy/iDustPrev.w) * float2(0.5, -0.5);";
    if (!InsAfter(s, from, body, vel))
    {
        // Variant without the INITIALISE_OUTPUT anchor (some material variants, e.g. hair_fs, init
        // the GBuffer differently). oDustVel depends only on the injected inputs, so writing it at
        // function start is valid — and the most alpha-clip-safe position. Never silently ship a
        // MV-less PS for a shader this table promised to inject.
        if (!InsAtFuncStart(s, fnAnchor, vel)) return src;
    }
    return s;
}

// Per-shader anchor tables, keyed by SOURCE FILE + entry point. Filename disambiguates the many
// .hlsl that share the "main_vs"/"main_ps"/"main_fs" entry names (objects/terrain/skin/birds/...).
struct VSpec { const char* srcFile; const char* entry; const char* fnAnchor; const char* interp; const char* clipAssign; const char* clipRHS; bool pad; };
struct PSpec { const char* srcFile; const char* entry; const char* fnAnchor; const char* interp; const char* gbufOut; const char* body; bool pad; };

// pad=true on the VSes whose meshes the item-icon pipeline can render (paired with the uninjected
// rtticons COLOURING PS): rigid items = objects.hlsl, severed limbs = character.hlsl. foliage
// farm_vs already outputs a real COLOR (no pad needed, and a second COLOR0 would not compile);
// the rest never render item icons.
static const VSpec kVSpecs[] = {
    { "objects.hlsl",     "main_vs",      "void main_vs",      "TEXCOORD5,", "oPosition = mul(worldViewProjMatrix, position);", "mul(worldViewProjMatrix, position)", true },
    { "terrain.hlsl",     "main_vs",      "void main_vs",      "TEXCOORD1,", "oPosition = mul(worldViewProjMatrix, position);", "mul(worldViewProjMatrix, position)", false },
    { "triplanar.hlsl",   "triplanar_vs", "void triplanar_vs", "TEXCOORD5,", "oPosition = mul(worldViewProjMatrix, position);", "mul(worldViewProjMatrix, position)", false },  // has a real COLOR0 out
    { "mapfeature.hlsl",  "feature_vs",   "void feature_vs",   "TEXCOORD4,", "oPosition = mul(worldViewProj, iPosition);",      "mul(worldViewProj, iPosition)", false },
    { "distant_town.hlsl","main_vs",      "void main_vs",      "TEXCOORD2,", "oPosition = mul(worldViewProjMatrix, position);", "mul(worldViewProjMatrix, position)", false },
    { "birds.hlsl",       "main_vs",      "void main_vs",      "TEXCOORD5,", "oPosition = mul(viewProjMatrix, position);",       "mul(viewProjMatrix, position)", false },
    { "foliage.hlsl",     "farm_vs",      "void farm_vs",      "TEXCOORD5,", "oPosition = mul(viewProjMatrix, float4(finalPos, 1));", "mul(viewProjMatrix, float4(finalPos, 1))", false },   // crops; has a real COLOR out
    { "character.hlsl",   "severed_limb_vs","void severed_limb_vs","TEXCOORD5,","oPosition = mul(worldViewProjMatrix, position);","mul(worldViewProjMatrix, position)", true },
};
// pad mirrors the paired VS spec's pad flag (register-routing symmetry — see InjPS).
// The skin/character/creature family uses ANCHOR placement ("TEXCOORD5,"): all pair with
// skin.hlsl main_vs but have different tail input counts, so only a shared-prefix anchor keeps
// iDustCur/iDustPrev register-aligned for every member (the 4ed6cf0 hair fix, in-game verified).
static const PSpec kPSpecs[] = {
    { "objects.hlsl",    "main_ps",       "void main_ps",       nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", true },   // pairs w/ objects main_vs (pad=true there)
    { "terrain.hlsl",    "simple_fs",     "void simple_fs",     nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // LOD terrain
    { "terrainfp4.hlsl", "main_fs",       "void main_fs",       nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // near terrain (pairs w/ terrain.hlsl main_vs)
    { "terrainfp4.hlsl", "mapfeature_fs", "void mapfeature_fs", nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // pairs w/ mapfeature feature_vs
    { "triplanar.hlsl",  "triplanar_ps",  "void triplanar_ps",  nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },
    { "mapfeature.hlsl", "terrain_fs",    "void terrain_fs",    nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },
    { "skin.hlsl",       "main_fs",       "void main_fs",       "TEXCOORD5,", "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // skinned clothes (skin.hlsl main_vs — family anchor)
    { "creature.hlsl",   "main_fs",       "void main_fs",       "TEXCOORD5,", "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // creatures/animals (skin.hlsl main_vs, BLOOD — family anchor)
    { "distant_town.hlsl","main_fs",      "void main_fs",       nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },
    { "foliage.hlsl",    "grass_fs",      "void grass_fs",      nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // grass
    { "foliage.hlsl",    "foliage_fs",    "void foliage_fs",    nullptr,      "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // crops (farm_vs)
    { "character.hlsl",  "main_fs",       "void main_fs",       "TEXCOORD5,", "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // character body (family anchor)
    { "character.hlsl",  "hair_fs",       "void hair_fs",       "TEXCOORD5,", "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // hair (family anchor; no INITIALISE_OUTPUT -> InsAtFuncStart fallback writes the velocity)
    { "character.hlsl",  "zero_fs",       "void zero_fs",       "TEXCOORD5,", "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // family anchor (sparse input list — misaligned even in vanilla; zero-detail only)
    { "character.hlsl",  "distant_fs",    "void distant_fs",    "TEXCOORD5,", "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", false },  // character LOD (family anchor)
    { "character.hlsl",  "severed_limb_fs","void severed_limb_fs",nullptr,    "out GBuffer buffer", "INITIALISE_OUTPUT( buffer );", true },   // pairs w/ severed_limb_vs (pad=true there; counts match)
};

// skin.hlsl main_vs needs TRUE animation MVs, not camera reproj: skin the vertex a second time with
// the PREVIOUS frame's bone palette (dust_prevBones) and previous VP (dust_prevVP), both from a CB at
// b12 (b13 is reproj; SM4 has no b14+). WEIGHTS=3, bones row-major (48B) — mirror skin.hlsl exactly.
static std::string InjSkinVS(const std::string& src)
{
    if (Has(src, "oDustPrev")) return src;
    std::string s = src;
    size_t fn = s.find("void main_vs");
    if (fn == std::string::npos) return src;
    s.insert(fn, "cbuffer DustPrevCB : register(b12) { row_major float3x4 dust_prevBones[60]; row_major float4x4 dust_prevVP; };\n"
                 "cbuffer DustMVCB : register(b13) { column_major float4x4 dust_reproj; };\n");
    size_t from = s.find("void main_vs");
    if (from == std::string::npos) return src;
    // FAMILY ANCHOR placement (after the shared TC0-5 prefix), matching every family PS in
    // kPSpecs. This pipeline routes VS->PS varyings BY REGISTER (live-proven 2026-07-20 — see
    // InjPS), and the family PSes have different tail input counts (hair_fs has no wet/blood), so
    // only a shared-prefix anchor keeps iDustCur/iDustPrev at v7/v8 <- o7/o8 for ALL of them
    // (the 4ed6cf0 hair fix, in-game verified). No COLOR0 pad here: per the ICONDRAW ground-truth
    // probe, the item-icon pipeline renders items with objects.hlsl main_vs + rtticons.hlsl —
    // skinned meshes (this VS) are used for the 128px character PORTRAITS, whose PSes are all
    // injected family members and therefore anchor-aligned.
    if (!InsAfter(s, from, "TEXCOORD5,", "\n\tout float4 oDustCur : TEXCOORD12,\n\tout float4 oDustPrev : TEXCOORD13,")) return src;
    std::string prev = std::string() +
        "\n\toDustCur = mul(viewProjectionMatrix, blendPos);"
        "\n\tfloat4 dmvPrev = float4(0,0,0,0);"
        "\n\t[unroll] for (int dmvI = 0; dmvI < 3; dmvI++) dmvPrev += float4(mul(dust_prevBones[blendIdx[dmvI]], position).xyz, 1.0) * blendWgt[dmvI];"
        "\n\toDustPrev = mul(dust_prevVP, dmvPrev);"
        // No prev pose bound this draw (first frame / unmatched / OGRE's reflected zero-buffer at b12)
        // => dust_prevVP is all-zero => fall back to CAMERA REPROJECTION (b13), not zero: a missed match
        // then looks like the character just didn't animate that frame (subtle) instead of didn't move at
        // all (a jarring flash under camera motion). Correct for a still character; ~right when moving.
        "\n\tif (dot(dust_prevVP._m30_m31_m32_m33, dust_prevVP._m30_m31_m32_m33) == 0.0) oDustPrev = mul(dust_reproj, oDustCur);";
    if (!InsAfter(s, from, "oPosition = mul(viewProjectionMatrix, blendPos);", prev)) return src;
    return s;
}

// foliage.hlsl grass_vs sways the top verts by direction*sin(time + x*frequency). Camera reprojection
// alone gives ~0 MV for a still camera, so the swaying grass over-accumulates in the upscaler and blurs.
// Inject TRUE wind MVs: recompute the sway at the PREVIOUS frame's time (the shader's own `time` uniform
// minus dust_windDelta, the host's frame-time delta riding in the b13 CB after dust_reproj), build that
// previous clip position, then camera-reproject it. dmvPrev mirrors grass_vs's own position math exactly.
static std::string InjGrassVS(const std::string& src)
{
    if (Has(src, "oDustPrev")) return src;
    std::string s = src;
    size_t fn = s.find("void grass_vs");
    if (fn == std::string::npos) return src;
    s.insert(fn, "cbuffer DustMVCB : register(b13) { column_major float4x4 dust_reproj; float dust_windDelta; };\n");
    size_t from = s.find("void grass_vs");
    if (from == std::string::npos) return src;
    if (!InsAtParamEnd(s, "void grass_vs", ",\n\tout float4 oDustCur : TEXCOORD12,\n\tout float4 oDustPrev : TEXCOORD13\n")) return src;
    from = s.find("void grass_vs");
    if (from == std::string::npos) return src;
    std::string prev =
        "\n\toDustCur = mul(worldViewProj, position);"
        "\n\tfloat4 dmvPrev = iPosition;"
        "\n\tif (iTexCoord.y == 0.0f) dmvPrev.xyz += direction * sin(time - dust_windDelta + iPosition.x * frequency);"
        "\n\tdmvPrev.y -= grassHeight * clamp(offset, 0, 1);"   // same distance fade as the current position
        "\n\toDustPrev = mul(dust_reproj, mul(worldViewProj, dmvPrev));";
    if (!InsAfter(s, from, "oPosition = mul(worldViewProj, position);", prev)) return src;
    return s;
}

// MV injection into a table-matched shader must never fail SILENTLY: an anchor miss ships the
// shader MV-less while everything around it is injected (the "hair has no velocity" bug class) —
// and if its counterpart stage WAS injected, interpolants mis-register (garbage MVs). The injector
// early-outs count as success (already patched); anything else that changed nothing gets a warning.
static std::string InjChecked(const std::string& out, const std::string& src,
                              const char* srcName, const char* entry, const char* marker)
{
    if (out == src && !Has(src, marker))
        Log("ShaderPatch: WARNING %s %s matched the MV-injection table but no anchors were found — shader ships WITHOUT motion vectors",
            srcName, entry);
    return out;
}

static std::string InjectGBufferVS(const std::string& src, const char* entry, const char* srcName)
{
    if (!srcName) return src;
    if (strcmp(entry, "main_vs") == 0 && Has(srcName, "skin.hlsl")) return InjChecked(InjSkinVS(src), src, srcName, entry, "oDustPrev");   // true anim MVs
    if (strcmp(entry, "grass_vs") == 0 && Has(srcName, "foliage.hlsl")) return InjChecked(InjGrassVS(src), src, srcName, entry, "oDustPrev");   // wind MVs
    // (objects.hlsl per-object previous-WVP / InjRigidVS is SHELVED — unreliable cross-frame identity
    //  regressed static geometry. objects.hlsl falls through to the generic camera-reproj InjVS below.)
    for (const VSpec& v : kVSpecs)
        if (strcmp(entry, v.entry) == 0 && Has(srcName, v.srcFile))
            return InjChecked(InjVS(src, v.fnAnchor, v.interp, v.clipAssign, v.clipRHS, v.pad), src, srcName, entry, "oDustPrev");
    return src;
}
static std::string InjectGBufferPS(const std::string& src, const char* entry, const char* srcName)
{
    if (!srcName) return src;
    for (const PSpec& ps : kPSpecs)
        if (strcmp(entry, ps.entry) == 0 && Has(srcName, ps.srcFile))
            return InjChecked(InjPS(src, ps.fnAnchor, ps.interp, ps.gbufOut, ps.body, ps.pad), src, srcName, entry, "oDustVel");
    return src;
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
                DumpInjection("deferred", pSourceName, pEntrypoint, src, patched, pDefines);
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, pTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    // Record shader source for survey (use patched source)
                    if (ppCode && *ppCode)
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName,
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
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

    // Detect the point/spot light-volume pixel shader: entry point "light_fs".
    // Apply direct-light AO so SSAO darkens local lights too, matching the sun pass.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "light_fs") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        if (src.find("attenuation * power") != std::string::npos &&
            src.find("aoMap") == std::string::npos)  // not already patched
        {
            std::string patched = PatchLightVolumeShader(src);
            if (patched.size() != src.size())
            {
                Log("ShaderPatch: patched deferred light_fs (%zu -> %zu bytes)",
                    src.size(), patched.size());
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, pTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    if (ppCode && *ppCode)
                    {
                        // Register the compiled blob so the host recognizes the resulting
                        // pixel shader at CreatePixelShader time and feeds it AO at s8/s9.
                        D3D11Hook::NoteLightVolumeShaderBytecode(
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName,
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                    }
                    return hr;
                }

                Log("ShaderPatch: patched light_fs failed to compile, falling back to original");
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

    // Inject per-object motion vectors into GBuffer shaders (velocity -> SV_Target3). Table-driven:
    // InjectGBufferVS/PS only rewrite when a shader's anchors match (see kVSpecs/kPSpecs), so all
    // unrelated shaders pass straight through. The objects foliage-alpha fix composes with the PS.
    if (pEntrypoint && pTarget && pSrcData && SrcDataSize > 0 && (pTarget[0] == 'v' || pTarget[0] == 'p'))
    {
        bool isVS = pTarget[0] == 'v';
        std::string src((const char*)pSrcData, SrcDataSize);
        std::string patched = src;
        // The alpha-dither fix targets IN-WORLD foliage. It must NOT touch the icon-rendering
        // shader (rtticons.hlsl): DustStabilizeThreshold snaps any alpha threshold outside
        // [0.24, 0.40] to 0.32, rewriting the icon pipeline's own threshold — icons come out
        // hollow/over-clipped or darkened (translucency rescale), or fully clipped to nothing
        // (the cross placeholder). The plain text match below can't tell the two pipelines
        // apart, so exclude the icon shader by source name.
        const bool isIconShader = pSourceName && strstr(pSourceName, "rtticons") != nullptr;
        if (!isVS && !isIconShader && patched.find("clip(normalTex.a - threshold)") != std::string::npos &&
            patched.find("DustStabilizeThreshold") == std::string::npos)
            patched = PatchObjectsShader(patched);
        // Item-icon quadrant fix (ROOT CAUSE, disasm-proven 2026-07-20): rtticons.hlsl main_ps
        // declares its COLOURING `colour : COLOR` input AFTER the DOUBLESIDED SV_IsFrontFace
        // parameter, so in DOUBLESIDED+COLOURING variants (mod cloth items) the SGV occupies v7
        // and pushes COLOR0 to v8. This pipeline routes varyings BY REGISTER: v8 reads VS output
        // o8 — one PAST vanilla's last output (COLOR0 sits at o7) — so vanilla renders these dyed
        // icons BLACK (known mod jank), and with Dust's injected VS o8 = oDustCur = clip position,
        // the white/cyan/blue/magenta quadrant tint. Moving the colour param to right after
        // TEXCOORD5 (before the SGV) puts COLOR0 at v7 <- o7 = the REAL dye colour: corrupted
        // icons get their true colours (better than vanilla's black). Variants without COLOURING
        // are byte-identical; COLOURING without DOUBLESIDED keeps COLOR0 at v7 as before. The
        // erase leaves the original #ifdef COLOURING block empty, which is legal.
        if (isIconShader && !isVS)
        {
            size_t fn = patched.find("float4 main_ps");   // rtticons main_ps RETURNS float4 (not void)
            if (fn != std::string::npos)
            {
                const char* colourDecl = "float4 colour : COLOR,";
                size_t t5 = patched.find("worldPos : TEXCOORD5,", fn);
                size_t c  = (t5 != std::string::npos) ? patched.find(colourDecl, t5) : std::string::npos;
                if (c != std::string::npos)
                {
                    patched.erase(c, strlen(colourDecl));
                    patched.insert(t5 + strlen("worldPos : TEXCOORD5,"),
                                   "\n#ifdef COLOURING\n\tfloat4 colour : COLOR,\n#endif");
                    Log("ShaderPatch: rtticons %s — COLOR input moved before SV_IsFrontFace (icon dye-colour fix)",
                        pEntrypoint);
                }
            }
        }
        // Diagnostic (with [Debug] DumpInjectedShaders=1): dump the icon shaders (patched form).
        if (isIconShader && DumpInjectedEnabled())
            DumpInjection(isVS ? "vs" : "ps", pSourceName, pEntrypoint, src, patched, pDefines);
        // Per-object MV injection rewrites the game's GBuffer VS/PS signatures, and it is ONLY consumed
        // by the DLSS/FSR upscaler resolve + the MV debug viz. When neither is active it is pure risk
        // for no benefit, so only inject when something actually reads the velocity. The linkage model
        // (RESOLVED 2026-07-20, fxc-verified): D3D11 links matched VS->PS semantics by NAME — but a PS
        // input with NO same-name VS output reads same-register garbage on NV. The "garbage item icons"
        // reports were the uninjected rtticons [COLOURING] PS (COLOR0 at input reg 7) paired with an
        // anchor-injected skin.hlsl main_vs that had shifted clip position into output reg 7; the
        // "world chroma" era was mixed-gate cached bytecode (gate now baked into the cache stamp, see
        // BuildCacheStamp). This gate MUST stay session-stable: it participates in the stamp.
        // (The alpha-dither fix above is independent — it stays.)
        bool mvInjected = false;
        if (D3D11Hook::MvInjectionWanted())
        {
            const size_t beforeMv = patched.size();
            patched = isVS ? InjectGBufferVS(patched, pEntrypoint, pSourceName) : InjectGBufferPS(patched, pEntrypoint, pSourceName);
            mvInjected = (patched.size() != beforeMv);
        }
        if (patched != src)
        {
            DumpInjection(isVS ? "vs" : "ps", pSourceName, pEntrypoint, src, patched, pDefines);
            HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                      pDefines, pInclude, pEntrypoint, pTarget,
                                      Flags1, Flags2, ppCode, ppErrorMsgs);
            if (SUCCEEDED(hr))
            {
                // Report what was ACTUALLY applied. This used to say "injected MV" for any rewrite,
                // which hid a gate-timing bug where the alpha-dither applied but the MV injection did
                // not — leaving MV-less pixel shaders paired with MV-injected vertex shaders.
                Log("ShaderPatch: patched %s %s (%zu -> %zu) [%s]",
                    pSourceName ? pSourceName : "?", pEntrypoint, src.size(), patched.size(),
                    mvInjected ? "MV" : "no MV");
                if (ppCode && *ppCode)
                    SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                        pEntrypoint, pTarget, pSourceName,
                        (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                return hr;
            }
            Log("ShaderPatch: MV inject failed to compile (%s), falling back", pEntrypoint);
            if (ppErrorMsgs && *ppErrorMsgs)
            {
                Log("ShaderPatch: error: %s", (const char*)(*ppErrorMsgs)->GetBufferPointer());
                (*ppErrorMsgs)->Release(); *ppErrorMsgs = nullptr;
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
