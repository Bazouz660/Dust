#include "ShaderPatch.h"
#include "DustLog.h"
#include "SurveyRecorder.h"
#include "D3D11Hook.h"

#include <string>
#include <cstring>

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
            // Slope bias = tan(angle to light) * vanilla-scale step. The NdotL
            // floor at 0.15 caps the term at ~6.6x its 45-degree value: with the
            // old 0.01 floor a grazing surface got a bias of ~0.1 depth units —
            // 3000x the vanilla shadow_bias (0.00003) — which un-shadowed
            // everything within hundreds of world units behind an occluder.
            "\tfloat sinSlope = sqrt(saturate(1.0 - NdotL * NdotL));\n"
            "\tb += dustRtwSlopeBias * sinSlope / max(NdotL, 0.15);\n"
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
            // Bias Scale adds multiples of the vanilla shadow_bias (0.00003,
            // rtwshadows.program). This must NOT be tied to the filter radius:
            // fr is a UV-space length, and the old `b += fr * biasScale` added
            // 0.001+ to a DEPTH bias — 33x vanilla — wiping out character-scale
            // self-shadowing (body-thickness depth deltas are ~0.00005), which
            // read as "glossy" characters lit through their own bodies.
            "\tfloat fr = dustRtwFilterRadius;\n"
            "\tfloat ls = dustRtwLightSize;\n"
            "\tb += 0.00003 * dustRtwBiasScale;\n"
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
            // Cap penumbra growth at 2x. The old 4x let the far cascades'
            // large depth deltas blow the estimate up into a smeary mess.
            "\t\t\tradius = clamp(pen, baseRadius * 0.5, baseRadius * 2.0);\n"
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
            // This is the FAR cascade (4-tap). Its base filter is already at
            // the ~1-texel resolution floor, and one texel here is several
            // meters of WORLD space (measured ~5.7m at split 375->4999), so
            // the base penumbra is already ~7x the near cascades'. Allowing
            // PCSS to grow it (the old 1.5x) only pushed that further past the
            // floor with no benefit — contact-hardening is meaningless when a
            // texel spans meters. Cap growth at 1.0x so PCSS may only tighten
            // (down to 0.5x), never soften past the texel-floor base.
            "\t\t\tradius = clamp(pen, baseRadius * 0.5, baseRadius * 1.0);\n"
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
            // 0.75 matches vanilla's effective footprint: vanilla scales its
            // hex offsets (avg magnitude ~2.07) by csmParams[i][1]*0.3; our
            // Poisson offsets (avg ~0.82) need ~0.75 to land at the same
            // average filter width. dustCsmFilterRadius (default 1.0) is the
            // user multiplier on top.
            "\tfloat baseRadius0 = csmParams[idx][1] * dustCsmFilterRadius * 0.75;\n"
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

    // Detect objects shader for foliage alpha fix: entry point is "main_ps"
    // and source contains the vanilla hard-cutoff alpha test.
    if (pEntrypoint && pSrcData && SrcDataSize > 0 &&
        strcmp(pEntrypoint, "main_ps") == 0)
    {
        std::string src((const char*)pSrcData, SrcDataSize);
        if (src.find("clip(normalTex.a - threshold)") != std::string::npos &&
            src.find("DustStabilizeThreshold") == std::string::npos)
        {
            std::string patched = PatchObjectsShader(src);
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
