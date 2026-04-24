#include "DeferredMSAA.h"
#include "MSAARedirect.h"
#include "D3D11StateBlock.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>

namespace DeferredMSAA
{

static ID3D11Device* sDevice = nullptr;
static UINT sWidth = 0, sHeight = 0;
static float sEdgeThreshold = 0.02f;

static ID3D11VertexShader* sVS = nullptr;
static ID3D11PixelShader* sPS = nullptr;
static uint32_t sCompiledSampleCount = 0;

static ID3D11Texture2D* sHDRCopyTex = nullptr;
static ID3D11ShaderResourceView* sHDRCopySRV = nullptr;

static ID3D11Buffer* sCorrectionCB = nullptr;
static ID3D11DepthStencilState* sNoDepthState = nullptr;

// Double-buffered staging for PS CB0 and VS CB0
static ID3D11Buffer* sPSStaging[2] = {};
static ID3D11Buffer* sVSStaging[2] = {};
static int sStagingSlot = 0;
static bool sStagingReady = false;
static uint32_t sPSCBSize = 0;

static D3D11StateBlock sStateBlock;
static bool sFirstLog = true;
static int sDebugMode = 0;

struct LightingParams {
    float sunDir[4];
    float sunCol[4];
    float fogParams[4];
    float envCol[4];
    float invView[16];
    float corner1[4];
    float corner2[4];
    bool valid;
};
static LightingParams sParams = {};

// CB layout sent to the shader (must match HLSL cbuffer)
struct CorrectionCBLayout {
    float sunDir[3];     float farClip;
    float sunCol[4];
    float envCol[4];
    float invView[16];
    float corner1[3];    float edgeThresh;
    float corner2[3];    uint32_t sampleCnt;
    float invRes[2];     float debugMode; float pad;
};
static_assert(sizeof(CorrectionCBLayout) == 160, "CB size mismatch");

// ---- Shader source ----

static const char* sVSSrc =
    "struct O { float4 p : SV_Position; };\n"
    "O main(uint id : SV_VertexID) {\n"
    "    O o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.p = float4(uv * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char* sPSSrc = R"(
Texture2DMS<float>  g_gbuf2 : register(t2);
Texture2D           g_hdrCopy : register(t4);

cbuffer CB : register(b0) {
    float3 g_sunDir;    float g_farClip;
    float4 g_sunCol;
    float4 g_envCol;
    float4x4 g_invView;
    float3 g_corner1;   float g_edgeThresh;
    float3 g_corner2;   uint  g_sampleCnt;
    float2 g_invRes;    float g_debugMode; float g_pad;
};

static const int2 kOff[8] = {
    int2(-1,0), int2(1,0), int2(0,-1), int2(0,1),
    int2(-1,-1), int2(1,-1), int2(-1,1), int2(1,1)
};

// For each sample, find the neighbor whose resolved depth best matches,
// and borrow that neighbor's vanilla lit color.
float3 ResolveSample(int2 c, float ds, float initDiff, float3 fallback) {
    float bestDiff = initDiff;
    float3 bestCol = fallback;
    [unroll] for (uint n = 0; n < 8; n++) {
        int2 nc = c + kOff[n];
        float nd = g_gbuf2.Load(nc, 0);
        if (nd > 0.00001) {
            float diff = abs(nd - ds);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestCol = g_hdrCopy.Load(int3(nc, 0)).rgb;
            }
        }
    }
    return bestCol;
}

float4 main(float4 pos : SV_Position) : SV_Target {
    int2 c = int2(pos.xy);
    float3 vanilla = g_hdrCopy.Load(int3(c, 0)).rgb;

    // Gather per-sample depth, find min/max valid depth
    float minD = 1e30, maxD = 0;
    uint valid = 0;
    float cd = 1e30;
    uint ci = 0;
    [unroll] for (uint i = 0; i < SAMPLE_COUNT; i++) {
        float d = g_gbuf2.Load(c, i);
        if (d > 0.00001) {
            minD = min(minD, d); maxD = max(maxD, d); valid++;
            if (d < cd) { cd = d; ci = i; }
        }
    }

    // No valid geometry at all
    if (valid == 0) return float4(vanilla, 1.0);

    // Partial-coverage edge (sky/geometry boundary)
    // Vanilla is corrupted: the game's chroma reconstruction on the resolved GBuffer
    // reads sky neighbors (zeroed chroma), producing purple/pink.
    // Distance-1 neighbors may also be corrupted (cascade). Search at distance 2-3.
    if (valid < SAMPLE_COUNT) {
        if (g_debugMode > 1.5 && g_debugMode < 2.5) return float4(1, 0, 0, 1);

        static const int2 kFar[16] = {
            int2(-2,0), int2(2,0), int2(0,-2), int2(0,2),
            int2(-2,-1), int2(2,-1), int2(-2,1), int2(2,1),
            int2(-1,-2), int2(1,-2), int2(-1,2), int2(1,2),
            int2(-3,0), int2(3,0), int2(0,-3), int2(0,3)
        };
        float3 result = 0;
        [unroll] for (uint s = 0; s < SAMPLE_COUNT; s++) {
            float ds = g_gbuf2.Load(c, s);
            if (ds < 0.00001) continue;
            float bestDiff = 1e30;
            float3 bestCol = vanilla;
            [unroll] for (uint n = 0; n < 16; n++) {
                int2 nc = c + kFar[n];
                float nd = g_gbuf2.Load(nc, 0);
                if (nd > 0.00001) {
                    float diff = abs(nd - ds);
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        bestCol = g_hdrCopy.Load(int3(nc, 0)).rgb;
                    }
                }
            }
            result += bestCol;
        }
        result /= float(valid);
        return float4(result, 1.0);
    }

    // Full-coverage: check depth edge threshold
    float rel = (maxD - minD) / max((minD + maxD) * 0.5, 0.0001);
    if (rel < g_edgeThresh) return float4(vanilla, 1.0);

    // Debug modes
    if (g_debugMode > 1.5 && g_debugMode < 2.5) return float4(0, 1, 0, 1);

    // Full-coverage edge: depth-aware neighbor resolve
    float3 result = 0;
    [unroll] for (uint s = 0; s < SAMPLE_COUNT; s++) {
        float ds = g_gbuf2.Load(c, s);
        result += ResolveSample(c, ds, abs(ds - cd), vanilla);
    }
    result /= float(SAMPLE_COUNT);

    if (g_debugMode > 3.5 && g_debugMode < 4.5) {
        float spread = rel * 10.0;
        return float4(spread, spread, spread, 1.0);
    }

    return float4(result, 1.0);
}
)";

// ---- Resource management ----

static void ReleaseShaders()
{
    if (sVS) { sVS->Release(); sVS = nullptr; }
    if (sPS) { sPS->Release(); sPS = nullptr; }
    sCompiledSampleCount = 0;
}

static void ReleaseResources()
{
    if (sHDRCopySRV) { sHDRCopySRV->Release(); sHDRCopySRV = nullptr; }
    if (sHDRCopyTex) { sHDRCopyTex->Release(); sHDRCopyTex = nullptr; }
    if (sCorrectionCB) { sCorrectionCB->Release(); sCorrectionCB = nullptr; }
    if (sNoDepthState) { sNoDepthState->Release(); sNoDepthState = nullptr; }
}

static void ReleaseStaging()
{
    for (int i = 0; i < 2; i++)
    {
        if (sPSStaging[i]) { sPSStaging[i]->Release(); sPSStaging[i] = nullptr; }
        if (sVSStaging[i]) { sVSStaging[i]->Release(); sVSStaging[i] = nullptr; }
    }
    sStagingReady = false;
    sPSCBSize = 0;
}

static bool CompileShaders(uint32_t sampleCount)
{
    if (sVS && sPS && sCompiledSampleCount == sampleCount)
        return true;

    ReleaseShaders();

    char countStr[8];
    snprintf(countStr, sizeof(countStr), "%u", sampleCount);
    D3D_SHADER_MACRO defines[] = {
        { "SAMPLE_COUNT", countStr },
        { nullptr, nullptr }
    };

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;

    HRESULT hr = D3DCompile(sVSSrc, strlen(sVSSrc), "DeferredMSAA_VS", nullptr, nullptr,
                            "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr))
    {
        if (errors) { Log("DeferredMSAA: VS compile error: %s", (const char*)errors->GetBufferPointer()); errors->Release(); }
        return false;
    }
    if (errors) { errors->Release(); errors = nullptr; }
    sDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sVS);
    blob->Release();
    if (!sVS) return false;

    hr = D3DCompile(sPSSrc, strlen(sPSSrc), "DeferredMSAA_PS", defines, nullptr,
                    "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr))
    {
        if (errors) { Log("DeferredMSAA: PS compile error: %s", (const char*)errors->GetBufferPointer()); errors->Release(); }
        ReleaseShaders();
        return false;
    }
    if (errors) { errors->Release(); errors = nullptr; }
    sDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sPS);
    blob->Release();
    if (!sPS) { ReleaseShaders(); return false; }

    sCompiledSampleCount = sampleCount;
    Log("DeferredMSAA: compiled shaders for %ux MSAA", sampleCount);
    return true;
}

static bool EnsureResources()
{
    if (!sDevice || sWidth == 0 || sHeight == 0) return false;

    if (!sHDRCopyTex)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = sWidth;
        desc.Height = sHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(sDevice->CreateTexture2D(&desc, nullptr, &sHDRCopyTex)))
            return false;
        if (FAILED(sDevice->CreateShaderResourceView(sHDRCopyTex, nullptr, &sHDRCopySRV)))
            { ReleaseResources(); return false; }
    }

    if (!sCorrectionCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(CorrectionCBLayout);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(sDevice->CreateBuffer(&bd, nullptr, &sCorrectionCB)))
            { ReleaseResources(); return false; }
    }

    if (!sNoDepthState)
    {
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        sDevice->CreateDepthStencilState(&ds, &sNoDepthState);
    }

    return true;
}

// ---- Public API ----

void SetDevice(ID3D11Device* device)
{
    if (sDevice != device)
    {
        ReleaseResources();
        ReleaseShaders();
        ReleaseStaging();
    }
    sDevice = device;
}

void SetResolution(UINT width, UINT height)
{
    if (width != sWidth || height != sHeight)
    {
        sWidth = width;
        sHeight = height;
        if (sHDRCopyTex) { sHDRCopySRV->Release(); sHDRCopySRV = nullptr; sHDRCopyTex->Release(); sHDRCopyTex = nullptr; }
    }
}

void CaptureLightingCB(ID3D11DeviceContext* ctx)
{
    // Capture PS CB0 (sun params, fog, inverseView)
    ID3D11Buffer* psCB = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCB);
    if (!psCB) return;

    D3D11_BUFFER_DESC cbDesc;
    psCB->GetDesc(&cbDesc);
    if (cbDesc.ByteWidth < 192) { psCB->Release(); return; }

    // Capture VS CB0 (frustum corners)
    ID3D11Buffer* vsCB = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &vsCB);
    if (!vsCB) { psCB->Release(); return; }

    D3D11_BUFFER_DESC vsDesc;
    vsCB->GetDesc(&vsDesc);
    if (vsDesc.ByteWidth < 32) { vsCB->Release(); psCB->Release(); return; }

    // Recreate staging if CB size changed
    if (sPSStaging[0] && sPSCBSize != cbDesc.ByteWidth)
        ReleaseStaging();

    // Create staging buffers on first call
    if (!sPSStaging[0])
    {
        D3D11_BUFFER_DESC sd = cbDesc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sDevice->CreateBuffer(&sd, nullptr, &sPSStaging[0]);
        sDevice->CreateBuffer(&sd, nullptr, &sPSStaging[1]);

        D3D11_BUFFER_DESC svd = vsDesc;
        svd.Usage = D3D11_USAGE_STAGING;
        svd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        svd.BindFlags = 0;
        svd.MiscFlags = 0;
        sDevice->CreateBuffer(&svd, nullptr, &sVSStaging[0]);
        sDevice->CreateBuffer(&svd, nullptr, &sVSStaging[1]);

        sPSCBSize = cbDesc.ByteWidth;
    }

    if (!sPSStaging[0] || !sVSStaging[0]) { psCB->Release(); vsCB->Release(); return; }

    int writeSlot = sStagingSlot;
    int readSlot = 1 - writeSlot;

    ctx->CopyResource(sPSStaging[writeSlot], psCB);
    ctx->CopyResource(sVSStaging[writeSlot], vsCB);
    psCB->Release();
    vsCB->Release();

    // Read previous frame's data (no GPU stall)
    if (sStagingReady)
    {
        D3D11_MAPPED_SUBRESOURCE psMap, vsMap;
        bool psOK = SUCCEEDED(ctx->Map(sPSStaging[readSlot], 0, D3D11_MAP_READ, 0, &psMap));
        bool vsOK = SUCCEEDED(ctx->Map(sVSStaging[readSlot], 0, D3D11_MAP_READ, 0, &vsMap));

        if (psOK && vsOK)
        {
            const float* ps = (const float*)psMap.pData;
            const float* vs = (const float*)vsMap.pData;

            memcpy(sParams.sunDir,    ps + 0,  16);  // c0
            memcpy(sParams.sunCol,    ps + 4,  16);  // c1
            memcpy(sParams.fogParams, ps + 8,  16);  // c2
            memcpy(sParams.envCol,    ps + 12, 16);  // c3
            memcpy(sParams.invView,   ps + 32, 64);  // c8-c11
            memcpy(sParams.corner1,   vs + 0,  16);  // VS c0
            memcpy(sParams.corner2,   vs + 4,  16);  // VS c1

            bool valid = true;
            for (int i = 0; i < 16; i++)
                if (!std::isfinite(sParams.invView[i])) { valid = false; break; }
            sParams.valid = valid;

            static bool sFirstCapture = true;
            if (sFirstCapture && valid)
            {
                Log("DeferredMSAA CB capture:");
                Log("  sunDir=(%.3f,%.3f,%.3f) sunCol=(%.3f,%.3f,%.3f,%.3f)",
                    sParams.sunDir[0], sParams.sunDir[1], sParams.sunDir[2],
                    sParams.sunCol[0], sParams.sunCol[1], sParams.sunCol[2], sParams.sunCol[3]);
                Log("  fogParams=(%.1f,%.3f,%.3f,%.3f) envCol=(%.3f,%.3f,%.3f,%.3f)",
                    sParams.fogParams[0], sParams.fogParams[1], sParams.fogParams[2], sParams.fogParams[3],
                    sParams.envCol[0], sParams.envCol[1], sParams.envCol[2], sParams.envCol[3]);
                Log("  corner1=(%.2f,%.2f,%.2f) corner2=(%.2f,%.2f,%.2f)",
                    sParams.corner1[0], sParams.corner1[1], sParams.corner1[2],
                    sParams.corner2[0], sParams.corner2[1], sParams.corner2[2]);
                Log("  invView c8=(%.4f,%.4f,%.4f,%.4f) c11=(%.4f,%.4f,%.4f,%.4f)",
                    sParams.invView[0], sParams.invView[1], sParams.invView[2], sParams.invView[3],
                    sParams.invView[12], sParams.invView[13], sParams.invView[14], sParams.invView[15]);
                sFirstCapture = false;
            }
        }

        if (psOK) ctx->Unmap(sPSStaging[readSlot], 0);
        if (vsOK) ctx->Unmap(sVSStaging[readSlot], 0);
    }

    sStagingSlot = readSlot;
    sStagingReady = true;
}

void Execute(ID3D11DeviceContext* ctx)
{
    if (sDebugMode == 1) return;

    uint32_t sampleCount = MSAARedirect::GetSampleCount();
    if (sampleCount < 2 || !sParams.valid || !sDevice)
        return;

    if (!EnsureResources()) return;
    if (!CompileShaders(sampleCount)) return;

    // Get MSAA SRVs
    ID3D11ShaderResourceView* msaaColor0 = MSAARedirect::GetColorSRV(0);
    ID3D11ShaderResourceView* msaaColor1 = MSAARedirect::GetColorSRV(1);
    ID3D11ShaderResourceView* msaaColor2 = MSAARedirect::GetColorSRV(2);
    ID3D11ShaderResourceView* msaaDepth  = MSAARedirect::GetDepthSRV();
    if (!msaaColor0 || !msaaColor1 || !msaaColor2 || !msaaDepth)
        return;

    // Get current HDR render target
    ID3D11RenderTargetView* hdrRTV = nullptr;
    ctx->OMGetRenderTargets(1, &hdrRTV, nullptr);
    if (!hdrRTV) return;

    // Copy HDR content for reading
    ID3D11Resource* hdrRes = nullptr;
    hdrRTV->GetResource(&hdrRes);
    if (hdrRes)
    {
        ctx->CopyResource(sHDRCopyTex, hdrRes);
        hdrRes->Release();
    }

    // Update correction CB
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(sCorrectionCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            CorrectionCBLayout* cb = (CorrectionCBLayout*)mapped.pData;
            memcpy(cb->sunDir, sParams.sunDir, 12);
            cb->farClip = sParams.fogParams[0];
            memcpy(cb->sunCol, sParams.sunCol, 16);
            memcpy(cb->envCol, sParams.envCol, 16);
            memcpy(cb->invView, sParams.invView, 64);
            memcpy(cb->corner1, sParams.corner1, 12);
            cb->edgeThresh = sEdgeThreshold;
            memcpy(cb->corner2, sParams.corner2, 12);
            cb->sampleCnt = sampleCount;
            cb->invRes[0] = 1.0f / sWidth;
            cb->invRes[1] = 1.0f / sHeight;
            cb->debugMode = (float)sDebugMode; cb->pad = 0;
            ctx->Unmap(sCorrectionCB, 0);
        }
    }

    sStateBlock.Capture(ctx);

    // Set render target (same HDR RT, no depth)
    ctx->OMSetRenderTargets(1, &hdrRTV, nullptr);
    ctx->OMSetDepthStencilState(sNoDepthState, 0);
    float blendFactor[4] = { 1, 1, 1, 1 };
    ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)sWidth;
    vp.Height = (float)sHeight;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(nullptr);

    // Bind SRVs: t0-t3 = MSAA GBuffer, t4 = vanilla HDR copy
    ID3D11ShaderResourceView* srvs[5] = { msaaColor0, msaaColor1, msaaColor2, msaaDepth, sHDRCopySRV };
    ctx->PSSetShaderResources(0, 5, srvs);
    ctx->PSSetConstantBuffers(0, 1, &sCorrectionCB);
    ctx->VSSetShader(sVS, nullptr, 0);
    ctx->PSSetShader(sPS, nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->Draw(3, 0);

    // Unbind SRVs (state block only restores 4)
    ID3D11ShaderResourceView* nullSRVs[5] = {};
    ctx->PSSetShaderResources(0, 5, nullSRVs);

    sStateBlock.Restore(ctx);
    hdrRTV->Release();

    if (sFirstLog)
    {
        Log("DeferredMSAA: first edge correction pass (%ux MSAA, %ux%u, threshold=%.4f)",
            sampleCount, sWidth, sHeight, sEdgeThreshold);
        sFirstLog = false;
    }
}

void SetEdgeThreshold(float threshold) { sEdgeThreshold = threshold; }
float GetEdgeThreshold() { return sEdgeThreshold; }
void SetDebugMode(int mode) { sDebugMode = mode; }
int GetDebugMode() { return sDebugMode; }

void Shutdown()
{
    ReleaseResources();
    ReleaseShaders();
    ReleaseStaging();
    sDevice = nullptr;
    sParams.valid = false;
    sFirstLog = true;
}

} // namespace DeferredMSAA
