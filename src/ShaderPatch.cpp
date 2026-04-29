#include "ShaderPatch.h"
#include "DustLog.h"
#include "SurveyRecorder.h"
#include "ShaderSourceCatalog.h"

#include <string>
#include <cstring>
#include <vector>

namespace ShaderPatch
{


typedef HRESULT(WINAPI* PFN_D3DCompileHook)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

PFN_D3DCompileHook oD3DCompile = nullptr;

// Diagnostic flags read once from Dust.ini for bisecting the deferred patch.
//   DisableShadowRedirect=1  → skip the RTWShadow → conditional ternary swap
//   DisableMSAAPatch=1       → skip MSAA-conditional GBuffer reads + Texture2DMS decls
//   DisableAOPatch=1         → skip AO injection (samplers + envLight *= ao)
//   DisableSpecAAPatch=1     → skip geometric specular AA
struct DustPatchFlags
{
    int disableShadowRedirect = 0;
    int disableMSAAPatch      = 0;
    int disableAOPatch        = 0;
    int disableSpecAAPatch    = 0;
};

static DustPatchFlags ReadPatchFlags()
{
    DustPatchFlags f = {};
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ReadPatchFlags, &hSelf);
    char selfPath[MAX_PATH] = {};
    if (hSelf) GetModuleFileNameA(hSelf, selfPath, MAX_PATH);
    char* slash = strrchr(selfPath, '\\');
    if (slash) { *(slash + 1) = '\0'; strcat_s(selfPath, "Dust.ini"); }
    f.disableShadowRedirect = GetPrivateProfileIntA("Dust", "DisableShadowRedirect", 0, selfPath);
    f.disableMSAAPatch      = GetPrivateProfileIntA("Dust", "DisableMSAAPatch",      0, selfPath);
    f.disableAOPatch        = GetPrivateProfileIntA("Dust", "DisableAOPatch",        0, selfPath);
    f.disableSpecAAPatch    = GetPrivateProfileIntA("Dust", "DisableSpecAAPatch",    0, selfPath);
    if (f.disableShadowRedirect) DustLog("ShaderPatch: shadow redirect DISABLED");
    if (f.disableMSAAPatch)      DustLog("ShaderPatch: MSAA patch DISABLED");
    if (f.disableAOPatch)        DustLog("ShaderPatch: AO patch DISABLED");
    if (f.disableSpecAAPatch)    DustLog("ShaderPatch: SpecAA patch DISABLED");
    return f;
}

// Patch vanilla deferred.hlsl source to add AO support and improved shadow filtering.
// Returns the modified source, or the original if patterns weren't found.
static std::string PatchDeferredShader(const std::string& src)
{
    static DustPatchFlags flags = ReadPatchFlags();
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
        "\tuniform sampler aoParams : register(s9),\n"
        "\tuniform sampler dustShadowMask : register(s13),\n\n\t";
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

    if (!flags.disableAOPatch)
    {
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
    }

    // === Specular AA Patch ===
    // Geometric specular anti-aliasing (Kaplanyan & Hill 2016): widen roughness
    // based on screen-space normal derivatives to eliminate specular flickering.
    // Injected before LightingData so it affects sun + point light specular.
    // IBL specular (CalcEnvironmentLight) runs earlier with original roughness,
    // which is fine — IBL is low-frequency and doesn't alias.

    const char* specAAAnchor = "LightingData ld = (LightingData)0.0f;";
    size_t specAAPos = result.find(specAAAnchor);
    if (specAAPos != std::string::npos && !flags.disableSpecAAPatch)
    {
        std::string specAA =
            "// [Dust] Geometric specular anti-aliasing\n"
            "\t{\n"
            "\t\tfloat3 _dNdx = ddx(normal);\n"
            "\t\tfloat3 _dNdy = ddy(normal);\n"
            "\t\tfloat _specAAVar = max(dot(_dNdx, _dNdx), dot(_dNdy, _dNdy));\n"
            "\t\tfloat _roughness = 1.0 - gloss;\n"
            "\t\t_roughness = sqrt(_roughness * _roughness + min(2.0 * _specAAVar, 0.18));\n"
            "\t\tgloss = 1.0 - _roughness;\n"
            "\t}\n\n\t";
        result.insert(specAAPos, specAA);
        Log("ShaderPatch: injected geometric specular AA");
    }

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
            "// [Dust] RT shadow mask — screen-space binary mask produced by DustShadowsRT.\n"
            "// When dustRTShadowEnabled > 0 the ray-traced result overrides the RTW shadow.\n"
            "cbuffer DustRTShadowCB : register(b4) {\n"
            "\tfloat dustRTShadowEnabled;\n"
            "\tfloat3 _dustRTShadowPad;\n"
            "};\n\n"
            "// [Dust] Shadow filtering parameters cbuffer reserved at b2 (currently unused\n"
            "// in the deferred patch — the Shadows effect plugin still binds it for legacy\n"
            "// reasons but no DustRTWShadow function is used in RTW mode).\n"
            "cbuffer DustShadowParams : register(b2) {\n"
            "\tfloat dustShadowEnabled;\n"
            "\tfloat dustFilterRadius;\n"
            "\tfloat dustLightSize;\n"
            "\tfloat dustPCSSEnabled;\n"
            "\tfloat dustBiasScale;\n"
            "\tfloat dustCliffFixEnabled;\n"
            "\tfloat dustCliffFixDistance;\n"
            "};\n\n"
            "#if 0  // DustRTWShadow disabled — see commit notes\n"
            "// [Dust] DustRTWShadow — currently a passthrough to vanilla RTWShadow.\n"
            "// The previous PCF/PCSS filter logic produced wrong values in RTWSM\n"
            "// (scene went shiny/metallic). Pending a proper rewrite, just call\n"
            "// vanilla so enabling the Shadows effect doesn't break rendering.\n"
            "// The extra params (screenPos, normal, dist, shadowRange) are kept\n"
            "// in the signature so the redirect call site doesn't need to change.\n"
            "float DustRTWShadow(sampler2D sMap, sampler2D wMap, float4x4 shadowMatrix,\n"
            "                     float3 worldPos, float b, float edgeBias, float2 screenPos,\n"
            "                     float3 normal, float dist, float shadowRange) {\n"
            "\treturn RTWShadow(sMap, wMap, shadowMatrix, worldPos, b, edgeBias);\n"
            "}\n"
            "#endif // outer DustRTWShadow guard\n"
            "// (legacy filter body retained below, but never reached)\n"
            "#if 0\n"
            "float _DustRTWShadowLegacy(sampler2D sMap, sampler2D wMap, float4x4 shadowMatrix,\n"
            "                     float3 worldPos, float b, float edgeBias, float2 screenPos,\n"
            "                     float3 normal, float dist, float shadowRange) {\n"
            "\tfloat4 sc = mul(shadowMatrix, float4(worldPos, 1));\n"
            "\t// Edge bias from the warped centre location (matches vanilla RTWShadow).\n"
            "\tfloat2 centerWarp = GetOffsetLocationS(wMap, sc.xy);\n"
            "\tfloat2 edge = saturate(abs(centerWarp - 0.5) * 20 - 9);\n"
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
            "\tfloat3 ld = normalize(shadowMatrix[2].xyz);\n"
            "\tfloat NdotL = abs(dot(normal, ld));\n"
            "\tfloat b_center = b;\n"
            "\tb += fr * dustBiasScale * (1.0 - NdotL);\n"
            "\n"
            "\tif (dustPCSSEnabled > 0.5) {\n"
            "\t\tfloat bSum = 0;\n"
            "\t\tfloat bCnt = 0;\n"
            "\t\t[unroll]\n"
            "\t\tfor (int j = 0; j < 12; j++) {\n"
            "\t\t\tfloat2 off = mul(rot, pd[j]) * ls;\n"
            "\t\t\tfloat2 sampleClip = sc.xy + off;\n"
            "\t\t\tfloat2 sampleUV   = GetOffsetLocationS(wMap, sampleClip);\n"
            "\t\t\tfloat dd = tex2Dlod(sMap, float4(sampleUV, 0, 0)).x;\n"
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
            "\tfr *= max(sqrt(NdotL), 0.15);\n"
            "\n"
            "\tfloat centerS = ShadowMap(sMap, center, sd, b_center, 0);\n"
            "\tfloat shadow = 0;\n"
            "\t[unroll]\n"
            "\tfor (int i = 0; i < 12; i++) {\n"
            "\t\tfloat2 off = mul(rot, pd[i]) * fr;\n"
            "\t\tfloat2 sampleClip = sc.xy + off;\n"
            "\t\tfloat2 sampleUV   = GetOffsetLocationS(wMap, sampleClip);\n"
            "\t\tshadow += ShadowMap(sMap, sampleUV, sd, b, 0);\n"
            "\t}\n"
            "\tshadow /= 12.0;\n"
            "\treturn min(shadow, centerS);\n"
            "}\n"
            "#endif\n"
            "\n";
        result.insert(pos3, inject3);
        Log("ShaderPatch: injected DustShadowParams cbuffer + DustRTWShadow function");
    }
    else
    {
        Log("ShaderPatch: anchor 'main_vs' not found, shadow function injection skipped");
    }

    // Injection 4: Replace RTWShadow call with conditional.
    // DISABLED BY DEFAULT — every form of "modify shadow with the tex2Dlod
    // result of dustShadowMask" produces shiny/metallic rendering in RTWSM
    // mode (extensive diagnostic bisection in this commit's session). The
    // codegen issue is mode-specific: same patches in CSM look fine. Until
    // that's understood, RTWSM uses vanilla shadow. Re-enable for testing
    // via Dust.ini  [Dust] EnableShadowRedirect=1.
    HMODULE _hSelf = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&PatchDeferredShader, &_hSelf);
    char _selfP[MAX_PATH] = {};
    if (_hSelf) GetModuleFileNameA(_hSelf, _selfP, MAX_PATH);
    char* _slash = strrchr(_selfP, '\\');
    if (_slash) { *(_slash + 1) = '\0'; strcat_s(_selfP, "Dust.ini"); }
    int _enableRedirect = GetPrivateProfileIntA("Dust", "EnableShadowRedirect", 0, _selfP);

    const char* callAnchor = "= RTWShadow(";
    size_t anchorPos = result.find(callAnchor);
    if (anchorPos != std::string::npos && _enableRedirect && !flags.disableShadowRedirect)
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

        // Originally a ternary, but ternaries are non-short-circuit in HLSL —
        // both arms are evaluated. DustRTWShadow has 24+ tex2Dlod calls and
        // its always-on evaluation produced wrong output on the vanilla
        // fallback path in RTWSM (scene went wet/metallic). Use [branch] if
        // statements so only the taken branch executes.
        //
        // Strategy: keep the original `shadow = RTWShadow(...);` as the
        // baseline, then append two conditional overrides. Find the trailing
        // ';' so we can splice in after it without disturbing the original.
        size_t stmtEnd = result.find(';', closeParen);
        if (stmtEnd != std::string::npos)
        {
            // RT shadow mask override via lerp (no if-statement). Use step()
            // to convert the float flag into a 0/1 lerp factor. FXC compiles
            // this as a straight cmov-equivalent without any [branch] / if
            // pattern that triggered the codegen issue we've been chasing.
            (void)params;
            std::string overrides =
                "\n\tfloat _dustRTMask = tex2Dlod(dustShadowMask, float4(texCoord, 0, 0)).r;"
                "\n\tshadow = lerp(shadow, _dustRTMask, step(0.5, dustRTShadowEnabled));";
            result.insert(stmtEnd + 1, overrides);
            Log("ShaderPatch: appended RT shadow mask override (lerp-based)");
            Log("ShaderPatch: original call: %s", originalCall.c_str());
            Log("ShaderPatch: original call: %s", originalCall.c_str());
        }
        else
        {
            Log("ShaderPatch: no trailing ';' found after RTWShadow call, redirect skipped");
        }
    }
    else
    {
        Log("ShaderPatch: '= RTWShadow(' not found, shadow redirect skipped");
    }

    // === MSAA Per-Sample Shading ===
    // When MSAA is active, the deferred draw targets an MSAA render target and the
    // GPU runs the pixel shader once per sample (driven by SV_SampleIndex). Each
    // invocation reads its own GBuffer sample from the MSAA textures, producing
    // correct per-sample lighting with proper shadow lookups (no divergent loops).

    const char* msaaAnchor1 = "void main_vs (";
    size_t msaaPos1 = result.find(msaaAnchor1);
    if (msaaPos1 != std::string::npos && !flags.disableMSAAPatch)
    {
        std::string msaaDecls =
            "// [Dust] MSAA per-sample shading\n"
            "Texture2DMS<float4> dustMSAA0 : register(t10);\n"
            "Texture2DMS<float4> dustMSAA1 : register(t11);\n"
            "Texture2DMS<float>  dustMSAA2 : register(t12);\n"
            "\n"
            "cbuffer DustMSAACB : register(b3) {\n"
            "\tuint dustMSAASamples;\n"
            "\tfloat dustMSAAPad0;\n"
            "\tfloat dustDebugMode;\n"
            "\tfloat dustMSAAPad1;\n"
            "};\n"
            "\n"
            "float3 DustDecodeAlbedoMS(int2 c, uint si) {\n"
            "\tfloat2 yg = dustMSAA0.Load(c, si).rg;\n"
            "\tfloat sd = dustMSAA2.Load(c, si).r;\n"
            "\tfloat thr = max(sd * 0.04, 0.001);\n"
            "\tfloat4 nd = float4(\n"
            "\t\tdustMSAA2.Load(c + int2(-1,0), si).r,\n"
            "\t\tdustMSAA2.Load(c + int2( 1,0), si).r,\n"
            "\t\tdustMSAA2.Load(c + int2(0,-1), si).r,\n"
            "\t\tdustMSAA2.Load(c + int2(0, 1), si).r);\n"
            "\tfloat4 nc = float4(\n"
            "\t\tdustMSAA0.Load(c + int2(-1,0), si).g,\n"
            "\t\tdustMSAA0.Load(c + int2( 1,0), si).g,\n"
            "\t\tdustMSAA0.Load(c + int2(0,-1), si).g,\n"
            "\t\tdustMSAA0.Load(c + int2(0, 1), si).g);\n"
            "\tfloat4 w = step(0.00001, nd) * (1.0 - step(thr, abs(nd - sd)));\n"
            "\tfloat W = dot(w, 1.0);\n"
            "\tfloat mis = (W > 0.0) ? dot(w, nc) / W : yg.g;\n"
            "\tbool ev = ((c.x & 1) == (c.y & 1));\n"
            "\tfloat Co = ev ? mis : yg.y;\n"
            "\tfloat Cg = ev ? yg.y : mis;\n"
            "\tCo -= 0.5; Cg -= 0.5;\n"
            "\treturn saturate(float3(yg.x + Co - Cg, yg.x + Cg, yg.x - Co - Cg));\n"
            "}\n\n";
        result.insert(msaaPos1, msaaDecls);
        Log("ShaderPatch: injected MSAA declarations + DustDecodeAlbedoMS");
    }

    // Inject SV_SampleIndex in main_fs parameter list.
    // Must go AFTER all TEXCOORD inputs to avoid shifting interpolator registers.
    // Search from main_fs to avoid matching main_vs's TEXCOORD1 output.
    if (!flags.disableMSAAPatch)
    {
        size_t mainFsPos = result.find("main_fs");
        const char* texcoordAnchor = ": TEXCOORD1,";
        size_t tcPos = (mainFsPos != std::string::npos)
            ? result.find(texcoordAnchor, mainFsPos) : std::string::npos;
        if (tcPos != std::string::npos)
        {
            size_t lineEnd = result.find('\n', tcPos);
            if (lineEnd != std::string::npos)
            {
                result.insert(lineEnd + 1, "\tuint dustSampleIdx : SV_SampleIndex,\n");
                Log("ShaderPatch: injected SV_SampleIndex in main_fs (after TEXCOORD1)");
            }
        }
    }

    // Replace GBuffer reads with MSAA-conditional versions.
    // When dustMSAASamples >= 2, read from Texture2DMS at the current sample.
    // When 0, use the original tex2D reads from the resolved single-sample GBuffer.
    if (!flags.disableMSAAPatch)
    {
        const char* albedoOld = "decodePixel(gBuf0, texCoord, viewport, pixel.xy)";
        size_t albedoPos = result.find(albedoOld);
        if (albedoPos != std::string::npos)
        {
            std::string albedoNew =
                "(dustMSAASamples >= 2 "
                "? DustDecodeAlbedoMS(int2(pixel.xy), dustSampleIdx) "
                ": decodePixel(gBuf0, texCoord, viewport, pixel.xy))";
            result.replace(albedoPos, strlen(albedoOld), albedoNew);
            Log("ShaderPatch: MSAA-conditional albedo read");
        }

        const char* metGlossOld = "tex2D(gBuf0, texCoord).ba";
        size_t metGlossPos = result.find(metGlossOld);
        if (metGlossPos != std::string::npos)
        {
            std::string metGlossNew =
                "(dustMSAASamples >= 2 "
                "? dustMSAA0.Load(int2(pixel.xy), dustSampleIdx).ba "
                ": tex2D(gBuf0, texCoord).ba)";
            result.replace(metGlossPos, strlen(metGlossOld), metGlossNew);
            Log("ShaderPatch: MSAA-conditional metalness/gloss read");
        }

        const char* normalOld = "tex2D(gBuf1, texCoord)";
        size_t normalPos = result.find(normalOld);
        if (normalPos != std::string::npos)
        {
            std::string normalNew =
                "(dustMSAASamples >= 2 "
                "? dustMSAA1.Load(int2(pixel.xy), dustSampleIdx) "
                ": tex2D(gBuf1, texCoord))";
            result.replace(normalPos, strlen(normalOld), normalNew);
            Log("ShaderPatch: MSAA-conditional normal read");
        }

        const char* depthOld = "tex2D(gBuf2, texCoord).r";
        size_t depthPos = result.find(depthOld);
        if (depthPos != std::string::npos)
        {
            std::string depthNew =
                "(dustMSAASamples >= 2 "
                "? dustMSAA2.Load(int2(pixel.xy), dustSampleIdx).r "
                ": tex2D(gBuf2, texCoord).r)";
            result.replace(depthPos, strlen(depthOld), depthNew);
            Log("ShaderPatch: MSAA-conditional depth read");
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

// Default CB/SRV bindings for the patched deferred shader.
// The shader patches reference dustShadowEnabled (b2), dustRTShadowEnabled (b4),
// aoMap (s8), aoParams (s9), and dustShadowMask (s13). When no plugin is
// active, those slots stay unbound and read garbage — `dustRTShadowEnabled`
// can land >0.5, triggering the RT shadow branch which then samples an unbound
// dustShadowMask (=0), tanking the sun term. Symptom: scene goes wet/metallic.
// CSM mode dodges this because its shadow path doesn't go through the patched
// ternary; RTW mode is hit hard.
//
// Fix: the framework binds known-safe defaults before any plugin runs.
} // namespace ShaderPatch (close before DustDeferredDefaults — kept at file scope
  //                       so D3D11Hook can reach it without qualification, matching
  //                       the pre-refactor layout)

namespace DustDeferredDefaults
{
    // Zero CB for b2 (DustShadowParams) and b4 (DustRTShadowCB). Both fields
    // we care about (dustShadowEnabled, dustRTShadowEnabled) read 0, so the
    // ternary picks the vanilla RTWShadow branch.
    static ID3D11Buffer*            sZeroCB        = nullptr;
    // 1x1 white texture for aoMap (s8) and dustShadowMask (s13) so reads
    // multiply by 1 (no tint) instead of 0 (kills env light).
    static ID3D11Texture2D*         sWhiteTex      = nullptr;
    static ID3D11ShaderResourceView* sWhiteSRV     = nullptr;
    // 1x1 black texture for aoParams (s9). directAO=0 → directFade=1 → sun
    // light unaffected.
    static ID3D11Texture2D*         sBlackTex      = nullptr;
    static ID3D11ShaderResourceView* sBlackSRV     = nullptr;
    static ID3D11SamplerState*       sPointSamp    = nullptr;

    static bool Ensure(ID3D11Device* device)
    {
        if (sZeroCB && sWhiteSRV && sBlackSRV && sPointSamp) return true;
        if (!device) return false;

        // 32-byte zero CB — covers DustShadowParams (5 floats) and DustRTShadowCB (4 floats).
        if (!sZeroCB)
        {
            D3D11_BUFFER_DESC cd = {};
            cd.ByteWidth      = 32;
            cd.Usage          = D3D11_USAGE_IMMUTABLE;
            cd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            uint32_t zeros[8] = {0};
            D3D11_SUBRESOURCE_DATA srd = { zeros, 0, 0 };
            if (FAILED(device->CreateBuffer(&cd, &srd, &sZeroCB)))
                return false;
        }

        auto make1x1 = [&](uint32_t color, ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSRV)
        {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = td.Height = 1;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd = { &color, 4, 0 };
            return SUCCEEDED(device->CreateTexture2D(&td, &srd, outTex)) &&
                   SUCCEEDED(device->CreateShaderResourceView(*outTex, nullptr, outSRV));
        };

        if (!sWhiteSRV && !make1x1(0xFFFFFFFFu, &sWhiteTex, &sWhiteSRV)) return false;
        if (!sBlackSRV && !make1x1(0x00000000u, &sBlackTex, &sBlackSRV)) return false;

        if (!sPointSamp)
        {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &sPointSamp))) return false;
        }
        return true;
    }

    void BindBeforeDeferred(ID3D11DeviceContext* ctx, ID3D11Device* device)
    {
        if (!Ensure(device)) return;
        // CBs at b2, b3, b4. b3 is DustMSAACB which DeferredMSAA::BindForLighting
        // only binds when MSAA is active — without that, dustMSAASamples reads
        // garbage and may drive the patched shader into the MSAA path (reads
        // unbound t10/t11/t12 → all 0). Always bind a zeroed default.
        ID3D11Buffer* cbs[1] = { sZeroCB };
        ctx->PSSetConstantBuffers(2, 1, cbs);
        ctx->PSSetConstantBuffers(3, 1, cbs);
        ctx->PSSetConstantBuffers(4, 1, cbs);
        // Textures + samplers at s8/s9/s13. Plugins (SSAO, ShadowsRT) may
        // overwrite in their PreExecute. Defaults: white at aoMap, black at
        // aoParams, white at dustShadowMask.
        ctx->PSSetShaderResources(8,  1, &sWhiteSRV);
        ctx->PSSetShaderResources(9,  1, &sBlackSRV);
        ctx->PSSetShaderResources(13, 1, &sWhiteSRV);
        ctx->PSSetSamplers(8,  1, &sPointSamp);
        ctx->PSSetSamplers(9,  1, &sPointSamp);
        ctx->PSSetSamplers(13, 1, &sPointSamp);

        static bool sFirstBind = true;
        if (sFirstBind)
        {
            DustLog("DustDeferredDefaults: bound zero CB at b2/b3/b4, white at s8/s13, black at s9");
            sFirstBind = false;
        }
    }

    void Shutdown()
    {
        if (sZeroCB)     { sZeroCB->Release();     sZeroCB = nullptr; }
        if (sWhiteSRV)   { sWhiteSRV->Release();   sWhiteSRV = nullptr; }
        if (sWhiteTex)   { sWhiteTex->Release();   sWhiteTex = nullptr; }
        if (sBlackSRV)   { sBlackSRV->Release();   sBlackSRV = nullptr; }
        if (sBlackTex)   { sBlackTex->Release();   sBlackTex = nullptr; }
        if (sPointSamp)  { sPointSamp->Release();  sPointSamp = nullptr; }
    }
}

namespace ShaderPatch
{

// Build a NULL-terminated array of macro names from a D3D_SHADER_MACRO list.
// Returned vector keeps the names alive; the array of pointers references
// the strings inside it. Pass through nullptr/empty as a single-element
// {nullptr} array.
static std::vector<const char*> CollectDefineNames(const D3D_SHADER_MACRO* pDefines)
{
    std::vector<const char*> names;
    if (pDefines)
        for (const D3D_SHADER_MACRO* m = pDefines; m && m->Name; ++m)
            names.push_back(m->Name);
    names.push_back(nullptr); // terminator
    return names;
}

HRESULT WINAPI HookedD3DCompile(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs)
{
    // Diagnostic: skip deferred shader patches entirely if Dust.ini has
    //   [Dust] DisableDeferredPatch=1
    // Lets us bisect "is the bug in the patch?" without rebuilding.
    static int sDisableDeferredPatchInit = 0;
    static int sDisableDeferredPatch = 0;
    if (!sDisableDeferredPatchInit)
    {
        char modPath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, modPath, MAX_PATH); // best-effort; we just need any nearby ini
        // Try Dust.ini next to the host module
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&HookedD3DCompile, &hSelf);
        char selfPath[MAX_PATH] = {};
        if (hSelf) GetModuleFileNameA(hSelf, selfPath, MAX_PATH);
        char* slash = strrchr(selfPath, '\\');
        if (slash) { *(slash + 1) = '\0'; strcat_s(selfPath, "Dust.ini"); }
        sDisableDeferredPatch = GetPrivateProfileIntA("Dust", "DisableDeferredPatch", 0, selfPath);
        sDisableDeferredPatchInit = 1;
        if (sDisableDeferredPatch)
            DustLog("ShaderPatch: deferred patch DISABLED via Dust.ini DisableDeferredPatch=1");
    }

    // Detect the deferred lighting pixel shader: entry point is "main_fs"
    // and source contains deferred-specific identifiers.
    if (!sDisableDeferredPatch && pEntrypoint && pSrcData && SrcDataSize > 0 &&
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
                const char* msaaTarget = (patched.find("dustMSAA0") != std::string::npos) ? "ps_5_0" : pTarget;
                HRESULT hr = oD3DCompile(patched.c_str(), patched.size(), pSourceName,
                                          pDefines, pInclude, pEntrypoint, msaaTarget,
                                          Flags1, Flags2, ppCode, ppErrorMsgs);
                if (SUCCEEDED(hr))
                {
                    Log("ShaderPatch: compiled deferred as %s", msaaTarget);
                    if (ppCode && *ppCode)
                    {
                        auto defineNames = CollectDefineNames(pDefines);
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName, defineNames.data(),
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
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
                    {
                        auto defineNames = CollectDefineNames(pDefines);
                        SurveyRecorder::OnShaderCompiled(patched.c_str(), patched.size(),
                            pEntrypoint, pTarget, pSourceName, defineNames.data(),
                            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
                    }
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
        auto defineNames = CollectDefineNames(pDefines);
        SurveyRecorder::OnShaderCompiled(pSrcData, SrcDataSize,
            pEntrypoint, pTarget, pSourceName, defineNames.data(),
            (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());

        // Phase 1.C: cross-validate the source-level catalog against the
        // bytecode reflection. Logs once per compile with a name+def summary.
        if (pSourceName && pEntrypoint)
        {
            std::vector<std::string> defs;
            for (const char* const* p = defineNames.data(); *p; ++p)
                defs.emplace_back(*p);
            ShaderSourceCatalog::ValidateAgainstBytecode(
                pSourceName, pEntrypoint, defs,
                (*ppCode)->GetBufferPointer(), (*ppCode)->GetBufferSize());
        }
    }

    return hr;
}

} // namespace ShaderPatch
