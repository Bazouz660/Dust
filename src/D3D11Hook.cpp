#include "D3D11Hook.h"
#include "DustGUI.h"
#include "PipelineDetector.h"
#include "EffectLoader.h"
#include "ResourceRegistry.h"
#include "ShaderMetadata.h"
#include "ShaderDatabase.h"
#include "ShaderSourceCatalog.h"
#include "GeometryCapture.h"
#include "MSAARedirect.h"
#include "DeferredMSAA.h"

#include "Survey.h"
#include "SurveyRecorder.h"
#include "SurveyWriter.h"
#include "DustLog.h"
#include <core/Functions.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <string>
#include <cstring>
#include <vector>

namespace D3D11Hook
{

// ==================== Global state ====================

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
bool gDeviceCaptured = false;

static UINT gWidth = 0;
static UINT gHeight = 0;
static uint64_t gFrameIndex = 0;
static bool gDispatchedThisFrame = false;

// Camera extraction from the game's deferred lighting CB
// (staging CBs are in gCameraStagingCBs[], declared near ExtractCameraData)
static DustCameraData gCameraData = {};
static bool gCameraDataExtracted = false; // per-frame flag


// VTable indices for swap chain methods (used by both Install() and deferred hooking)
static const int VTIDX_SC_Present        = 8;
static const int VTIDX_SC_ResizeBuffers  = 13;
static const int VTIDX_SC1_Present1      = 22;

// Deferred Present hooking: addresses saved from temp device, installed later
static void* sSavedAddrPresent   = nullptr;
static void* sSavedAddrPresent1  = nullptr;
static void* sSavedAddrResizeBuf = nullptr;
static bool  sSwapChainHooked    = false;

// Survey: collected frame data for writing after all frames captured
static std::vector<SurveyFrameData> sSurveyFrames;

static bool gDeviceRemovedThisFrame = false;
static volatile bool gShutdownSignaled = false;

void ResetFrameState()
{
    GeometryCapture::ResetFrame();
    gPipelineDetector.ResetFrame();
    gResourceRegistry.ResetFrame();

    gDispatchedThisFrame = false;
    gCameraDataExtracted = false;
    gDeviceRemovedThisFrame = false;
    gFrameIndex++;
}

// Extract camera basis vectors from the game's deferred lighting PS constant buffer.
// The inverse view matrix sits at register c8 (offset 128 bytes / 32 floats).
// Camera axes are the COLUMNS of the inverse view matrix (= rows of the view matrix).
// Double-buffered staging: CopyResource into slot N this frame, Map slot N-1
// (which the GPU finished long ago). Eliminates the CPU-GPU sync stall that
// was blocking the pipeline every frame.
static ID3D11Buffer* gCameraStagingCBs[2] = {};
static int gCameraStagingSlot = 0;
static bool gCameraStagingReady = false; // false until first copy has been issued

static void ExtractCameraData(ID3D11DeviceContext* ctx)
{
    if (gCameraDataExtracted) return;

    ID3D11Buffer* psCB = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCB);
    if (!psCB) return;

    D3D11_BUFFER_DESC cbDesc;
    psCB->GetDesc(&cbDesc);
    if (cbDesc.ByteWidth < 192) { psCB->Release(); return; }

    if (gCameraStagingCBs[0])
    {
        D3D11_BUFFER_DESC stagingDesc;
        gCameraStagingCBs[0]->GetDesc(&stagingDesc);
        if (stagingDesc.ByteWidth != cbDesc.ByteWidth)
        {
            gCameraStagingCBs[0]->Release();
            gCameraStagingCBs[1]->Release();
            gCameraStagingCBs[0] = nullptr;
            gCameraStagingCBs[1] = nullptr;
            gCameraStagingReady = false;
        }
    }

    if (!gCameraStagingCBs[0])
    {
        D3D11_BUFFER_DESC sd = cbDesc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        gDevice->CreateBuffer(&sd, nullptr, &gCameraStagingCBs[0]);
        gDevice->CreateBuffer(&sd, nullptr, &gCameraStagingCBs[1]);
    }
    if (!gCameraStagingCBs[0] || !gCameraStagingCBs[1]) { psCB->Release(); return; }

    int writeSlot = gCameraStagingSlot;
    int readSlot = 1 - writeSlot;

    // Copy this frame's CB into the write slot (async, no stall)
    ctx->CopyResource(gCameraStagingCBs[writeSlot], psCB);
    psCB->Release();

    // Read LAST frame's data from the other slot (GPU finished it long ago)
    if (gCameraStagingReady)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(gCameraStagingCBs[readSlot], 0, D3D11_MAP_READ, 0, &mapped)))
        {
            float m[16];
            memcpy(m, (float*)mapped.pData + 32, 64); // c8 offset
            ctx->Unmap(gCameraStagingCBs[readSlot], 0);

            bool valid = true;
            for (int i = 0; i < 16; i++)
                if (!isfinite(m[i])) { valid = false; break; }

            if (valid)
            {
                memcpy(gCameraData.inverseView, m, 64);
                gCameraData.camRight[0]   = m[0]; gCameraData.camRight[1]   = m[4]; gCameraData.camRight[2]   = m[8];
                gCameraData.camUp[0]      = m[1]; gCameraData.camUp[1]      = m[5]; gCameraData.camUp[2]      = m[9];
                gCameraData.camForward[0] = m[2]; gCameraData.camForward[1] = m[6]; gCameraData.camForward[2] = m[10];
                gCameraData.camPosition[0] = m[12]; gCameraData.camPosition[1] = m[13]; gCameraData.camPosition[2] = m[14];
                gCameraData.valid = 1;
                gCameraDataExtracted = true;
            }
        }
    }

    gCameraStagingSlot = readSlot;
    gCameraStagingReady = true;

}

// ==================== Original function pointers ====================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(
    ID3D11Device* pThis, const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePixelShader)(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader);

typedef void(STDMETHODCALLTYPE* PFN_Draw)(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(
    ID3D11DeviceContext* pThis, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView);

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUAVs)(
    ID3D11DeviceContext* pThis, UINT NumRTVs,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static PFN_CreateTexture2D          oCreateTexture2D = nullptr;
static PFN_CreatePixelShader        oCreatePixelShader = nullptr;
static PFN_Draw                     oDraw = nullptr;
static PFN_DrawIndexed              oDrawIndexed = nullptr;
static PFN_DrawIndexedInstanced     oDrawIndexedInstanced = nullptr;
static PFN_DrawInstanced            oDrawInstanced = nullptr;
static PFN_OMSetRenderTargets       oOMSetRenderTargets = nullptr;
static PFN_OMSetRenderTargetsAndUAVs oOMSetRenderTargetsAndUAVs = nullptr;
static PFN_Present                  oPresent = nullptr;
static PFN_ResizeBuffers            oResizeBuffers = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(
    IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters);
static PFN_Present1                 oPresent1 = nullptr;

// Hook-race diagnostics: if Present is bypassed by an overlay or a Present1 path,
// Draw fires but Present never does. These let us confirm that from the log.
static uint64_t gDrawHookCallCount = 0;
static uint64_t gPresentHookCallCount = 0;
static bool gGuiInitDone = false;

// ==================== D3DCompile hook (runtime shader patching) ====================

typedef HRESULT(WINAPI* PFN_D3DCompileHook)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

static PFN_D3DCompileHook oD3DCompile = nullptr;

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

    static void BindBeforeDeferred(ID3D11DeviceContext* ctx, ID3D11Device* device)
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

    static void Shutdown()
    {
        if (sZeroCB)     { sZeroCB->Release();     sZeroCB = nullptr; }
        if (sWhiteSRV)   { sWhiteSRV->Release();   sWhiteSRV = nullptr; }
        if (sWhiteTex)   { sWhiteTex->Release();   sWhiteTex = nullptr; }
        if (sBlackSRV)   { sBlackSRV->Release();   sBlackSRV = nullptr; }
        if (sBlackTex)   { sBlackTex->Release();   sBlackTex = nullptr; }
        if (sPointSamp)  { sPointSamp->Release();  sPointSamp = nullptr; }
    }
}

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

static HRESULT WINAPI HookedD3DCompile(
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

// ==================== DustBoot integration ====================
// DustBoot is a preload plugin that hooks IDXGIFactory::CreateSwapChain before
// the game creates its D3D11 device. If present, it provides the swap chain pointer
// directly — no runtime discovery needed.

typedef IDXGISwapChain* (*PFN_DustBoot_GetSwapChain)();
typedef HWND            (*PFN_DustBoot_GetHWND)();
typedef bool            (*PFN_DustBoot_IsHooked)();

static bool sTryCaptureFromBoot = false; // true if DustBoot provided the swap chain

static IDXGISwapChain* TryGetSwapChainFromBoot()
{
    // Called per-draw — throttle all failure messages to avoid log spam.
    static int sFailLogCount = 0;

    HMODULE boot = GetModuleHandleA("DustBoot.dll");
    if (!boot)
    {
        if (sFailLogCount < 1)
        { Log("DustBoot: not loaded (preload plugin not installed)"); ++sFailLogCount; }
        return nullptr;
    }

    auto isHooked = (PFN_DustBoot_IsHooked)GetProcAddress(boot, "DustBoot_IsHooked");
    if (!isHooked || !isHooked())
    {
        if (sFailLogCount < 3)
        { Log("DustBoot: loaded but factory hooks not active"); ++sFailLogCount; }
        return nullptr;
    }

    auto getSC = (PFN_DustBoot_GetSwapChain)GetProcAddress(boot, "DustBoot_GetSwapChain");
    if (!getSC)
    {
        if (sFailLogCount < 1)
        { Log("DustBoot: export DustBoot_GetSwapChain not found"); ++sFailLogCount; }
        return nullptr;
    }

    IDXGISwapChain* sc = getSC();
    if (!sc)
    {
        if (sFailLogCount < 3)
        { Log("DustBoot: hooked but swap chain not captured yet"); ++sFailLogCount; }
        return nullptr;
    }

    Log("DustBoot: swap chain captured at %p", sc);
    return sc;
}

// ==================== Deferred swap chain hooking ====================

// Forward declarations for hooks defined later (needed by TryInstallSwapChainHooks)
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pThis, UINT SyncInterval,
    UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* pThis, UINT BufferCount,
    UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

// Try to find the game's real swap chain by walking DXGI from the current
// render target. If the RT is the swap chain's back buffer, IDXGISurface::GetParent
// returns the swap chain. Falls back gracefully if the RT is an intermediate buffer.
// If ctx is null, falls back to gContext.
static bool TryDiscoverSwapChain(IDXGISwapChain** ppSwapChain, ID3D11DeviceContext* ctx = nullptr)
{
    *ppSwapChain = nullptr;

    if (!ctx) ctx = gContext;
    if (!ctx) return false;

    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv)
        return false;

    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (!res)
        return false;

    IDXGISurface* surface = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(IDXGISurface), (void**)&surface);
    res->Release();
    if (FAILED(hr) || !surface)
        return false;

    hr = surface->GetParent(__uuidof(IDXGISwapChain), (void**)ppSwapChain);
    surface->Release();

    if (FAILED(hr) || !*ppSwapChain)
        return false;

    Log("SwapChain discovery: found real swap chain at %p", *ppSwapChain);
    return true;
}

// VTable hook: directly replace function pointer in the COM object's vtable.
// Immune to inline hook conflicts from overlays (Steam, Discord, ReShade).
static bool VTableHook(void* pObject, int vtableIndex, void* detour, void** original)
{
    void** vtable = *reinterpret_cast<void***>(pObject);
    *original = vtable[vtableIndex];

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[vtableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    vtable[vtableIndex] = detour;
    VirtualProtect(&vtable[vtableIndex], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// The swap chain pointer we vtable-hooked — needed for recovery verification
static IDXGISwapChain* gHookedSwapChain = nullptr;

static void TryInstallSwapChainHooks(ID3D11DeviceContext* drawCtx = nullptr)
{
    if (sSwapChainHooked)
        return;

    // Layer 1: Try DustBoot (preload plugin that intercepted CreateSwapChain)
    IDXGISwapChain* realSwapChain = TryGetSwapChainFromBoot();
    if (realSwapChain)
    {
        sTryCaptureFromBoot = true;
        Log("Using swap chain from DustBoot (preload capture)");
    }
    else
    {
        // Layer 2: Fall back to runtime discovery from current render target.
        // Uses drawCtx if provided (any draw call), otherwise needs gContext from device capture.
        ID3D11DeviceContext* ctx = drawCtx ? drawCtx : gContext;
        if (!ctx)
            return;
        if (!TryDiscoverSwapChain(&realSwapChain, ctx) || !realSwapChain)
            return;
        Log("Using swap chain from runtime discovery (fallback)");
    }

    bool ok = true;

    // VTable hook Present (index 8)
    if (!VTableHook(realSwapChain, VTIDX_SC_Present, (void*)HookedPresent, (void**)&oPresent))
    { Log("ERROR: Failed to vtable-hook Present"); ok = false; }
    else
    { Log("  Present vtable-hooked on swap chain %p", realSwapChain); }

    // VTable hook ResizeBuffers (index 13)
    if (!VTableHook(realSwapChain, VTIDX_SC_ResizeBuffers, (void*)HookedResizeBuffers, (void**)&oResizeBuffers))
    { Log("ERROR: Failed to vtable-hook ResizeBuffers"); ok = false; }
    else
    { Log("  ResizeBuffers vtable-hooked on swap chain %p", realSwapChain); }

    // VTable hook Present1 (index 22 on IDXGISwapChain1)
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(realSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1)
    {
        if (!VTableHook(sc1, VTIDX_SC1_Present1, (void*)HookedPresent1, (void**)&oPresent1))
        { Log("WARNING: Failed to vtable-hook Present1"); }
        else
        { Log("  Present1 vtable-hooked"); }
        sc1->Release();
    }

    gHookedSwapChain = realSwapChain;
    // Both paths hold a reference: discovery via GetParent AddRef, DustBoot via explicit AddRef.
    realSwapChain->Release();
    sSwapChainHooked = true;

    if (ok)
        Log("All swap chain hooks installed successfully (via %s)",
            sTryCaptureFromBoot ? "DustBoot preload" : "runtime discovery");
    else
        Log("WARNING: Some swap chain hooks failed — GUI may not work");
}

// ==================== Present hook diagnostics ====================

bool IsPresentHooked()
{
    return sSwapChainHooked && gPresentHookCallCount > 0;
}

void TryRecoverPresent()
{
    if (gPresentHookCallCount > 0)
        return; // Already working

    Log("RECOVER: Attempting swap chain capture (DustBoot → discovery → vtable re-patch)...");
    sSwapChainHooked = false;
    TryInstallSwapChainHooks();

    if (!sSwapChainHooked)
    {
        Log("RECOVER: Re-hook failed. GUI will not be available this session. Effects still work.");
    }
}

// ==================== Device capture ====================

static void TryCaptureDevice(ID3D11Device* device)
{
    if (gDeviceCaptured)
        return;

    gDeviceCaptured = true;

    gDevice = device;
    device->GetImmediateContext(&gContext);
    GeometryCapture::SetDevice(device);
    MSAARedirect::SetDevice(device);
    DeferredMSAA::SetDevice(device);


    Log("Captured real D3D11 device=%p, context=%p", gDevice, gContext);

    // Try to get resolution from current RT
    {
        ID3D11RenderTargetView* rtv = nullptr;
        gContext->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv)
        {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                if (tex)
                {
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);
                    gWidth = desc.Width;
                    gHeight = desc.Height;
                    tex->Release();
                }
                res->Release();
            }
            rtv->Release();
        }
    }

    if (gWidth == 0 || gHeight == 0)
    {
        gWidth = 1;
        gHeight = 1;
        Log("Resolution not yet available, will detect from HDR RT on first frame");
    }
    else
    {
        Log("Detected resolution: %ux%u", gWidth, gHeight);
        GeometryCapture::SetResolution(gWidth, gHeight);
        MSAARedirect::SetResolution(gWidth, gHeight);
    }

    // Initialize all loaded effect plugins
    if (!gEffectLoader.InitAll(gDevice, gWidth, gHeight))
        Log("WARNING: One or more effect plugins failed to initialize");

    // Deferred: install Present/ResizeBuffers hooks now that the real device is captured.
    // By waiting until the first Draw call, we avoid the race with overlay DLLs
    // (Steam, Discord, ReShade) that also hook Present during their initialization.
    TryInstallSwapChainHooks();

    Log("Dust fully initialized and active");
}

// ==================== Hook implementations ====================

static bool IsPowerOf2(UINT v) { return v && !(v & (v - 1)); }

static const char* FormatName(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT:     return "R32_FLOAT";
        case DXGI_FORMAT_R32_TYPELESS:  return "R32_TYPELESS";
        case DXGI_FORMAT_R16_FLOAT:     return "R16_FLOAT";
        case DXGI_FORMAT_R16_UNORM:     return "R16_UNORM";
        case DXGI_FORMAT_R16_TYPELESS:  return "R16_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS:return "R24G8_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT:     return "D32_FLOAT";
        case DXGI_FORMAT_D16_UNORM:     return "D16_UNORM";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        default: return nullptr;
    }
}

static HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
    ID3D11Device* pThis, const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    if (pDesc && pDesc->Width == pDesc->Height &&
        IsPowerOf2(pDesc->Width) && pDesc->Width >= 256 && pDesc->Width <= 8192)
    {
        const char* fn = FormatName(pDesc->Format);
        if (fn)
        {
            bool hasSRV = (pDesc->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
            bool hasRTV = (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
            bool hasDSV = (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
            Log("CreateTex2D: %ux%u %s bind=%s%s%s",
                pDesc->Width, pDesc->Height, fn,
                hasSRV ? "SRV " : "", hasRTV ? "RTV " : "", hasDSV ? "DSV " : "");

        }
    }

    return oCreateTexture2D(pThis, pDesc, pInitialData, ppTexture2D);
}

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
{
    // NOTE: Do NOT capture device here.  OGRE (and other middleware) may create
    // temporary enumeration devices that are destroyed before rendering begins.
    // Device capture happens in HookedDraw from the actual rendering context.

    HRESULT hr = oCreatePixelShader(pThis, pShaderBytecode, BytecodeLength,
                                     pClassLinkage, ppPixelShader);
    if (SUCCEEDED(hr) && ppPixelShader && *ppPixelShader)
    {
        SurveyRecorder::OnPixelShaderCreated(pShaderBytecode, BytecodeLength, *ppPixelShader);
        ShaderDatabase::OnPixelShaderCreated(*ppPixelShader);
    }
    return hr;
}

// ==================== CreateVertexShader hook (for shader source tracking) ====================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVertexShader)(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader);

static PFN_CreateVertexShader oCreateVertexShader = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateVertexShader(
    ID3D11Device* pThis, const void* pShaderBytecode, SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader)
{
    HRESULT hr = oCreateVertexShader(pThis, pShaderBytecode, BytecodeLength,
                                      pClassLinkage, ppVertexShader);
    if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader)
    {
        SurveyRecorder::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
        ShaderMetadata::OnVertexShaderCreated(pShaderBytecode, BytecodeLength, *ppVertexShader);
        ShaderDatabase::OnVertexShaderCreated(*ppVertexShader);
    }
    return hr;
}

static bool sInMSAAResolve = false;


static void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* pThis, UINT VertexCount, UINT StartVertexLocation)
{
    if (gShutdownSignaled) { oDraw(pThis, VertexCount, StartVertexLocation); return; }

    // Try to install swap chain hooks early. DustBoot may already have captured the
    // swap chain, and we don't need device capture for that path. Pass pThis so
    // runtime discovery can also work from any draw call (not just fullscreen ones).
    if (!sSwapChainHooked)
        TryInstallSwapChainHooks(pThis);

    // Survey: record ALL draws (before fullscreen filter)
    if (Survey::IsActive())
        SurveyRecorder::OnDraw(pThis, VertexCount, StartVertexLocation);

    // DIAGNOSTIC: log if a non-indexed Draw call ever occurs inside the GBuffer pass.
    // We currently don't capture these — they'd be silently missed in wireframe replay.
    if (GeometryCapture::IsInGBufferPass())
    {
        static bool sLoggedNonIndexedInGBuffer = false;
        if (!sLoggedNonIndexedInGBuffer)
        {
            sLoggedNonIndexedInGBuffer = true;
            DustLog("D3D11Hook: NON-INDEXED Draw inside GBuffer pass — verts=%u start=%u (NOT captured for wireframe)",
                    VertexCount, StartVertexLocation);
        }
    }

    if (VertexCount != 3 && VertexCount != 4)
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
        return;
    }

    // Capture device from the rendering context on first fullscreen draw.
    // This is the actual game device, not a temporary enumeration device.
    if (!gDeviceCaptured)
    {
        ID3D11Device* ctxDevice = nullptr;
        pThis->GetDevice(&ctxDevice);
        if (ctxDevice)
        {
            TryCaptureDevice(ctxDevice);
            ctxDevice->Release();
        }

        if (!gDeviceCaptured)
        {
            oDraw(pThis, VertexCount, StartVertexLocation);
            return;
        }
    }

    // Check device health once per frame, not per draw call.
    // GetDeviceRemovedReason can cause driver synchronization on some hardware.
    if (!gDeviceRemovedThisFrame)
    {
        HRESULT removeReason = gDevice->GetDeviceRemovedReason();
        if (removeReason != S_OK)
        {
            Log("Device removed (0x%08X), skipping draw hook entirely", removeReason);
            gDeviceRemovedThisFrame = true;
        }
    }
    if (gDeviceRemovedThisFrame)
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
        return;
    }

    // Detect render pass from GPU state
    auto result = gPipelineDetector.OnFullscreenDraw(pThis);

    if (result.detected)
    {
        // Verify the context's device matches our captured device (one-time check)
        {
            static bool sDeviceChecked = false;
            if (!sDeviceChecked)
            {
                ID3D11Device* ctxDevice = nullptr;
                pThis->GetDevice(&ctxDevice);
                if (ctxDevice)
                {
                    if (ctxDevice != gDevice)
                        Log("WARNING: Context device=%p differs from captured device=%p!", ctxDevice, gDevice);
                    else
                        Log("Device pointer verified OK (device=%p)", gDevice);
                    ctxDevice->Release();
                }
                sDeviceChecked = true;
            }
        }

        // Detect real resolution from the HDR render target — only on the
        // lighting pass (first detection per frame) to avoid redundant COM calls.
        if (result.point == InjectionPoint::POST_LIGHTING)
        {
            ID3D11RenderTargetView* rtv = nullptr;
            pThis->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv)
            {
                ID3D11Resource* res = nullptr;
                rtv->GetResource(&res);
                if (res)
                {
                    ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                    if (tex)
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        tex->GetDesc(&desc);
                        if (desc.Width != gWidth || desc.Height != gHeight)
                        {
                            Log("Resolution changed: %ux%u -> %ux%u", gWidth, gHeight, desc.Width, desc.Height);
                            gWidth = desc.Width;
                            gHeight = desc.Height;
                            gEffectLoader.OnResolutionChanged(gDevice, gWidth, gHeight);
                            GeometryCapture::SetResolution(gWidth, gHeight);
                            MSAARedirect::SetResolution(gWidth, gHeight);
                                            }
                        tex->Release();
                    }
                    res->Release();
                }
                rtv->Release();
            }
        }

        if (gWidth <= 1 || gHeight <= 1)
        {
            Log("Skipping effect dispatch (res=%ux%u)", gWidth, gHeight);
            oDraw(pThis, VertexCount, StartVertexLocation);
            return;
        }

        DustInjectionPoint dip = static_cast<DustInjectionPoint>(result.point);

        // Log first successful dispatch
        {
            static bool sFirstDispatch = true;
            if (sFirstDispatch)
            {
                Log("First effect dispatch: point=%d, res=%ux%u, frame=%llu",
                    (int)dip, gWidth, gHeight, gFrameIndex);
                sFirstDispatch = false;
            }
        }

        // Extract camera data at POST_LIGHTING (deferred CB is bound).
        // Begin per-sample MSAA draw BEFORE BindForLighting so the CB
        // knows whether per-sample shading is actually active.
        if (dip == static_cast<DustInjectionPoint>(InjectionPoint::POST_LIGHTING))
        {
            ExtractCameraData(pThis);
            if (MSAARedirect::GetSampleCount() >= 2)
            {
                sInMSAAResolve = true;
                DeferredMSAA::BeginPerSampleDraw(pThis);
                sInMSAAResolve = false;
                DeferredMSAA::BindForLighting(pThis);
            }
            // Framework defaults for the patched deferred CBs/SRVs. Must run
            // before plugin PreExecute so plugins can overwrite where active.
            DustDeferredDefaults::BindBeforeDeferred(pThis, gDevice);
        }

        DustFrameContext fctx = {};
        fctx.device = gDevice;
        fctx.context = pThis;
        fctx.point = dip;
        fctx.width = gWidth;
        fctx.height = gHeight;
        fctx.frameIndex = gFrameIndex;
        fctx.camera = gCameraData;

        // PRE: effects bind resources before the game's draw
        fctx.timing = DUST_TIMING_PRE;
        gEffectLoader.DispatchPre(dip, &fctx);

        // Execute the game's original draw call
        oDraw(pThis, VertexCount, StartVertexLocation);

        if (dip == static_cast<DustInjectionPoint>(InjectionPoint::POST_LIGHTING))
        {
            if (MSAARedirect::GetSampleCount() >= 2)
            {
                sInMSAAResolve = true;
                DeferredMSAA::EndPerSampleDraw(pThis);
                sInMSAAResolve = false;
                DeferredMSAA::UnbindAfterLighting(pThis);
            }
        }

        // Snapshot pre-fog HDR right after the lighting draw completes.
        // Captured here so POST_FOG / POST_TONEMAP effects (e.g. Debug Views) can
        // still read the lit scene before fog/atmosphere were composited.
        if (dip == static_cast<DustInjectionPoint>(InjectionPoint::POST_LIGHTING))
            gEffectLoader.CapturePreFogHDR(pThis);

        // POST: effects that operate after the draw
        fctx.timing = DUST_TIMING_POST;
        gEffectLoader.DispatchPost(dip, &fctx);
    }
    else
    {
        oDraw(pThis, VertexCount, StartVertexLocation);
    }
}

static void STDMETHODCALLTYPE HookedDrawIndexed(
    ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    if (gShutdownSignaled) { oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation); return; }

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    // DIAG: if any DrawIndexed comes from a non-immediate context, deferred contexts
    // are in use and our sInGBufferPass tracking won't work correctly across them.
    if (gContext && pThis != gContext)
    {
        static bool sLoggedDeferred = false;
        if (!sLoggedDeferred)
        {
            sLoggedDeferred = true;
            DustLog("D3D11Hook: DrawIndexed on DEFERRED context %p (immediate=%p) — captures may be incomplete",
                    pThis, gContext);
        }
    }

    GeometryCapture::OnDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);

    if (MSAARedirect::IsActive())
        MSAARedirect::OnDraw();

    oDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void STDMETHODCALLTYPE HookedDrawInstanced(
    ID3D11DeviceContext* pThis, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    if (gShutdownSignaled) {
        oDrawInstanced(pThis, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        return;
    }

    if (GeometryCapture::IsInGBufferPass())
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            sLogged = true;
            DustLog("D3D11Hook: DrawInstanced inside GBuffer pass — verts=%u inst=%u (NOT captured for wireframe)",
                    VertexCountPerInstance, InstanceCount);
        }
    }

    oDrawInstanced(pThis, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
    ID3D11DeviceContext* pThis, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    if (gShutdownSignaled) { oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); return; }

    if (Survey::IsActive())
        SurveyRecorder::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                                StartIndexLocation, BaseVertexLocation,
                                                StartInstanceLocation);

    GeometryCapture::OnDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                                            StartIndexLocation, BaseVertexLocation,
                                            StartInstanceLocation);

    if (MSAARedirect::IsActive())
        MSAARedirect::OnDraw();

    oDrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                          StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(
    ID3D11DeviceContext* pThis, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView)
{
    if (gShutdownSignaled) { oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView); return; }
    if (sInMSAAResolve)
    {
        oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
        return;
    }

    bool wasInGBuffer = GeometryCapture::IsInGBufferPass();
    bool isGBuffer = GeometryCapture::CheckGBufferConfig(NumViews, ppRenderTargetViews, pDepthStencilView);

    if (MSAARedirect::GetSampleCount() >= 2)
    {
        if (!wasInGBuffer && isGBuffer)
        {
            ID3D11RenderTargetView* msaaRTVs[3];
            ID3D11DepthStencilView* msaaDSV;
            if (MSAARedirect::OnGBufferEnter(pThis, ppRenderTargetViews, pDepthStencilView, msaaRTVs, &msaaDSV))
            {
                oOMSetRenderTargets(pThis, NumViews, msaaRTVs, msaaDSV);
                GeometryCapture::OnOMSetRenderTargetsWithResult(isGBuffer);
                return;
            }
        }
        else if (wasInGBuffer && !isGBuffer && MSAARedirect::IsActive())
        {
            sInMSAAResolve = true;
            MSAARedirect::OnGBufferLeave(pThis);
            sInMSAAResolve = false;
        }
    }

    oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
    GeometryCapture::OnOMSetRenderTargetsWithResult(isGBuffer);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargetsAndUAVs(
    ID3D11DeviceContext* pThis, UINT NumRTVs,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts)
{
    if (gShutdownSignaled) { oOMSetRenderTargetsAndUAVs(pThis, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts); return; }
    if (sInMSAAResolve)
    {
        oOMSetRenderTargetsAndUAVs(pThis, NumRTVs, ppRenderTargetViews, pDepthStencilView,
                                   UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
        return;
    }

    // 0xffffffff = D3D11_KEEP_RENDER_TARGETS_UNCHANGED (RTVs aren't changing)
    if (NumRTVs == 0xffffffff)
    {
        oOMSetRenderTargetsAndUAVs(pThis, NumRTVs, ppRenderTargetViews, pDepthStencilView,
                                   UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
        return;
    }

    bool wasInGBuffer = GeometryCapture::IsInGBufferPass();
    bool isGBuffer = GeometryCapture::CheckGBufferConfig(NumRTVs, ppRenderTargetViews, pDepthStencilView);

    if (MSAARedirect::GetSampleCount() >= 2)
    {
        if (!wasInGBuffer && isGBuffer)
        {
            ID3D11RenderTargetView* msaaRTVs[3];
            ID3D11DepthStencilView* msaaDSV;
            if (MSAARedirect::OnGBufferEnter(pThis, ppRenderTargetViews, pDepthStencilView, msaaRTVs, &msaaDSV))
            {
                oOMSetRenderTargetsAndUAVs(pThis, NumRTVs, msaaRTVs, msaaDSV,
                                           UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
                GeometryCapture::OnOMSetRenderTargetsWithResult(isGBuffer);
                return;
            }
        }
        else if (wasInGBuffer && !isGBuffer && MSAARedirect::IsActive())
        {
            sInMSAAResolve = true;
            MSAARedirect::OnGBufferLeave(pThis);
            sInMSAAResolve = false;
        }
    }

    oOMSetRenderTargetsAndUAVs(pThis, NumRTVs, ppRenderTargetViews, pDepthStencilView,
                               UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    GeometryCapture::OnOMSetRenderTargetsWithResult(isGBuffer);
}

// ==================== Swap chain hooks (ImGui) ====================

static void TickGuiOnPresent(IDXGISwapChain* swapChain, const char* via)
{
    ++gPresentHookCallCount;

    // Periodic diagnostic: confirm Present is firing and show init state
    if (gPresentHookCallCount <= 5 ||
        (gPresentHookCallCount <= 600 && (gPresentHookCallCount % 60) == 0))
    {
        Log("Present #%llu via %s: captured=%d guiDone=%d boot=%d draws=%llu",
            (unsigned long long)gPresentHookCallCount, via,
            (int)gDeviceCaptured,
            (int)gGuiInitDone,
            (int)sTryCaptureFromBoot,
            (unsigned long long)gDrawHookCallCount);
    }

    // Survey: finalize frame at Present boundary
    if (Survey::IsActive())
    {
        SurveyFrameData frameData = SurveyRecorder::OnEndFrame();
        SurveyWriter::WriteFrame(frameData, Survey::GetOutputDir());
        sSurveyFrames.push_back(std::move(frameData));

        if (Survey::OnFrameEnd())
        {
            // Survey just finished — write shaders and summary
            SurveyWriter::WriteShaders(Survey::GetOutputDir());
            SurveyWriter::WriteSummary(sSurveyFrames.data(), (int)sSurveyFrames.size(),
                                        Survey::GetOutputDir());
            sSurveyFrames.clear();
            SurveyRecorder::Shutdown();
        }
    }

    if (!gGuiInitDone)
    {
        if (DustGUI::Init(swapChain, gDevice, gContext))
        {
            gGuiInitDone = true;
            Log("GUI initialized successfully (swap chain via %s)",
                sTryCaptureFromBoot ? "DustBoot preload" : "runtime discovery");
        }
        else
        {
            static int sRetryCount = 0;
            if (sRetryCount < 3)
                Log("GUI init failed, will retry (attempt %d)", ++sRetryCount);
        }
    }

    DustGUI::Render();
}

static HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags)
{
    if (!gShutdownSignaled) TickGuiOnPresent(pThis, "Present");
    return oPresent(pThis, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (!gShutdownSignaled) TickGuiOnPresent(pThis, "Present1");
    return oPresent1(pThis, SyncInterval, PresentFlags, pPresentParameters);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (gShutdownSignaled) return oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // Block Render() while we tear down and recreate the back buffer
    DustGUI::SetResizeInProgress(true);

    // Release only the back buffer RTV — keep ImGui context, WndProc, DInput hooks intact
    DustGUI::ReleaseBackBuffer();

    HRESULT hr = oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // Recreate back buffer RTV with the new swapchain dimensions
    if (SUCCEEDED(hr) && gGuiInitDone)
        DustGUI::RecreateBackBuffer(pThis);

    DustGUI::SetResizeInProgress(false);
    return hr;
}

// ==================== Install ====================

static const int VTIDX_DEVICE_CreateTexture2D       = 5;
static const int VTIDX_DEVICE_CreateVertexShader    = 12;
static const int VTIDX_DEVICE_CreatePixelShader     = 15;
static const int VTIDX_CTX_DrawIndexed              = 12;
static const int VTIDX_CTX_Draw                     = 13;
static const int VTIDX_CTX_DrawIndexedInstanced     = 20;
static const int VTIDX_CTX_DrawInstanced            = 21;
static const int VTIDX_CTX_OMSetRenderTargets       = 33;
static const int VTIDX_CTX_OMSetRenderTargetsAndUAVs = 34;
// VTIDX_SC_* constants moved to top of file (needed by deferred hook code)

bool Install()
{
    Log("Creating temporary D3D11 device + swap chain to discover function addresses...");

    // Need a dummy window for the swap chain
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "DustDummy";
    wc.hInstance = GetModuleHandleA(nullptr);
    RegisterClassExA(&wc);
    HWND dummyWnd = CreateWindowExA(0, "DustDummy", "", WS_OVERLAPPED,
                                     0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummyWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    IDXGISwapChain* tmpSwapChain = nullptr;
    ID3D11Device* tmpDevice = nullptr;
    ID3D11DeviceContext* tmpContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &tmpSwapChain, &tmpDevice, nullptr, &tmpContext);

    if (FAILED(hr))
    {
        Log("ERROR: Failed to create temporary D3D11 device: 0x%08X", hr);
        DestroyWindow(dummyWnd);
        UnregisterClassA("DustDummy", wc.hInstance);
        return false;
    }

    void** devVtable = *reinterpret_cast<void***>(tmpDevice);
    void** ctxVtable = *reinterpret_cast<void***>(tmpContext);
    void** scVtable  = *reinterpret_cast<void***>(tmpSwapChain);

    void* addrCreateTex2D  = devVtable[VTIDX_DEVICE_CreateTexture2D];
    void* addrCreateVS     = devVtable[VTIDX_DEVICE_CreateVertexShader];
    void* addrCreatePS     = devVtable[VTIDX_DEVICE_CreatePixelShader];
    void* addrDraw         = ctxVtable[VTIDX_CTX_Draw];
    void* addrDrawInst     = ctxVtable[VTIDX_CTX_DrawInstanced];
    void* addrDrawIndexed  = ctxVtable[VTIDX_CTX_DrawIndexed];
    void* addrDrawIdxInst  = ctxVtable[VTIDX_CTX_DrawIndexedInstanced];
    void* addrOMSetRT      = ctxVtable[VTIDX_CTX_OMSetRenderTargets];
    void* addrOMSetRTUAV   = ctxVtable[VTIDX_CTX_OMSetRenderTargetsAndUAVs];
    void* addrPresent      = scVtable[VTIDX_SC_Present];
    void* addrResizeBuf    = scVtable[VTIDX_SC_ResizeBuffers];

    // Try to find Present1 (IDXGISwapChain1, DXGI 1.2). Many flip-model
    // swap chains route through this and bypass Present entirely.
    void* addrPresent1 = nullptr;
    {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(tmpSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1)
        {
            void** sc1Vtable = *reinterpret_cast<void***>(sc1);
            addrPresent1 = sc1Vtable[VTIDX_SC1_Present1];
            sc1->Release();
        }
    }

    Log("Function addresses discovered:");
    Log("  CreateVertexShader    = %p", addrCreateVS);
    Log("  CreatePixelShader     = %p", addrCreatePS);
    Log("  Draw                  = %p", addrDraw);
    Log("  DrawIndexed           = %p", addrDrawIndexed);
    Log("  DrawIndexedInstanced  = %p", addrDrawIdxInst);
    Log("  OMSetRenderTargets    = %p", addrOMSetRT);
    Log("  OMSetRTsAndUAVs       = %p", addrOMSetRTUAV);
    Log("  Present               = %p", addrPresent);
    Log("  Present1              = %p", addrPresent1);
    Log("  ResizeBuffers         = %p", addrResizeBuf);

    tmpSwapChain->Release();
    tmpContext->Release();
    tmpDevice->Release();
    DestroyWindow(dummyWnd);
    UnregisterClassA("DustDummy", wc.hInstance);

    // Hook D3DCompile for runtime shader patching (no disk writes).
    // Kenshi ships D3DCompiler_43.dll (used by OGRE's RenderSystem_Direct3D11).
    // Must hook before OGRE compiles any shaders.
    Log("Installing D3D11 function hooks...");

    bool ok = true;

    {
        const char* compilerDlls[] = { "D3DCompiler_43.dll", "d3dcompiler_47.dll" };
        bool hooked = false;
        for (const char* dllName : compilerDlls)
        {
            // GetModuleHandle first (already loaded by RenderSystem), LoadLibrary as fallback
            HMODULE hD3DCompiler = GetModuleHandleA(dllName);
            if (!hD3DCompiler)
                hD3DCompiler = LoadLibraryA(dllName);
            if (!hD3DCompiler)
                continue;

            void* addrD3DCompile = (void*)GetProcAddress(hD3DCompiler, "D3DCompile");
            if (!addrD3DCompile)
                continue;

            if (KenshiLib::AddHook(addrD3DCompile, (void*)HookedD3DCompile,
                                   (void**)&oD3DCompile) == KenshiLib::SUCCESS)
            {
                Log("  D3DCompile hook installed via %s (runtime shader patching enabled)", dllName);
                hooked = true;
                break;
            }
        }
        if (!hooked)
        { Log("WARNING: Could not hook D3DCompile, shader patching disabled"); }
    }

    // Init survey defaults from INI
    {
        std::string ini = DustLogDir() + "Dust.ini";
        Survey::InitFromINI(ini.c_str());
    }

    if (KenshiLib::AddHook(addrCreateTex2D, (void*)HookedCreateTexture2D,
                           (void**)&oCreateTexture2D) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreateTexture2D"); ok = false; }

    if (KenshiLib::AddHook(addrCreateVS, (void*)HookedCreateVertexShader,
                           (void**)&oCreateVertexShader) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook CreateVertexShader (shader source tracking for VS disabled)"); }

    if (KenshiLib::AddHook(addrCreatePS, (void*)HookedCreatePixelShader,
                           (void**)&oCreatePixelShader) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook CreatePixelShader"); ok = false; }

    if (KenshiLib::AddHook(addrDraw, (void*)HookedDraw,
                           (void**)&oDraw) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook Draw"); ok = false; }

    if (KenshiLib::AddHook(addrDrawIndexed, (void*)HookedDrawIndexed,
                           (void**)&oDrawIndexed) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook DrawIndexed"); ok = false; }

    if (KenshiLib::AddHook(addrDrawIdxInst, (void*)HookedDrawIndexedInstanced,
                           (void**)&oDrawIndexedInstanced) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook DrawIndexedInstanced"); ok = false; }

    if (KenshiLib::AddHook(addrDrawInst, (void*)HookedDrawInstanced,
                           (void**)&oDrawInstanced) != KenshiLib::SUCCESS)
    { Log("WARNING: Failed to hook DrawInstanced (diagnostic only)"); }

    if (KenshiLib::AddHook(addrOMSetRT, (void*)HookedOMSetRenderTargets,
                           (void**)&oOMSetRenderTargets) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook OMSetRenderTargets"); ok = false; }

    if (KenshiLib::AddHook(addrOMSetRTUAV, (void*)HookedOMSetRenderTargetsAndUAVs,
                           (void**)&oOMSetRenderTargetsAndUAVs) != KenshiLib::SUCCESS)
    { Log("ERROR: Failed to hook OMSetRenderTargetsAndUnorderedAccessViews"); ok = false; }

    // Present/Present1/ResizeBuffers hooks are DEFERRED until the first Draw call.
    // This avoids a race with overlay DLLs (Steam, Discord, ReShade) that also hook
    // Present during their initialization. By hooking later, we wrap their hooks
    // and always fire. Addresses are saved and used in TryInstallSwapChainHooks().
    sSavedAddrPresent  = addrPresent;
    sSavedAddrPresent1 = addrPresent1;
    sSavedAddrResizeBuf = addrResizeBuf;
    Log("  Present hooks DEFERRED (Present=%p, Present1=%p, ResizeBuffers=%p)",
        addrPresent, addrPresent1, addrResizeBuf);

    if (ok)
        Log("Device/Context hooks installed, swap chain hooks deferred to first Draw");

    return ok;
}

void SignalShutdown()
{
    gShutdownSignaled = true;
    DustDeferredDefaults::Shutdown();
}

} // namespace D3D11Hook
