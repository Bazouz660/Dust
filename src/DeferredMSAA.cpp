#include "DeferredMSAA.h"
#include "MSAARedirect.h"
#include "D3D11StateBlock.h"
#include "DustLog.h"
#include <d3dcompiler.h>

namespace DeferredMSAA
{

static const DXGI_FORMAT MSAA_RT_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

static ID3D11Device* sDevice = nullptr;
static int sDebugMode = 0;

static ID3D11Buffer* sCB = nullptr;

struct alignas(16) MSAACBLayout {
    uint32_t sampleCount;
    float edgeThreshold;
    float debugMode;
    float pad;
};
static_assert(sizeof(MSAACBLayout) == 16, "CB must be 16 bytes");

// MSAA lighting render target (always RGBA16F for alpha coverage)
static ID3D11Texture2D*           sLightingMSAATex = nullptr;
static ID3D11RenderTargetView*    sLightingMSAARTV = nullptr;
static ID3D11ShaderResourceView*  sLightingMSAASRV = nullptr;
static UINT sLightingWidth = 0, sLightingHeight = 0;
static uint32_t sLightingSamples = 0;

// Resolve resources
static ID3D11VertexShader* sResolveVS = nullptr;
static ID3D11PixelShader*  sResolvePS = nullptr;
static ID3D11BlendState*   sBlendState = nullptr;
static D3D11StateBlock     sStateBlock;

// Saved state during per-sample draw
static ID3D11RenderTargetView* sSavedRTV = nullptr;
static ID3D11DepthStencilView* sSavedDSV = nullptr;
static bool sPerSampleActive = false;

static void ReleaseCB()
{
    if (sCB) { sCB->Release(); sCB = nullptr; }
}

static bool EnsureCB()
{
    if (!sDevice) return false;
    if (sCB) return true;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(MSAACBLayout);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(sDevice->CreateBuffer(&bd, nullptr, &sCB));
}

static void ReleaseResolve()
{
    if (sResolveVS) { sResolveVS->Release(); sResolveVS = nullptr; }
    if (sResolvePS) { sResolvePS->Release(); sResolvePS = nullptr; }
}

static void ReleaseLightingRT()
{
    if (sLightingMSAASRV) { sLightingMSAASRV->Release(); sLightingMSAASRV = nullptr; }
    if (sLightingMSAARTV) { sLightingMSAARTV->Release(); sLightingMSAARTV = nullptr; }
    if (sLightingMSAATex) { sLightingMSAATex->Release(); sLightingMSAATex = nullptr; }
    ReleaseResolve();
    sLightingWidth = sLightingHeight = 0;
    sLightingSamples = 0;
}

static bool CompileResolveShaders(uint32_t samples)
{
    if (sResolveVS && sResolvePS)
        return true;

    static const char* vsSrc =
        "struct O { float4 p : SV_Position; };\n"
        "O main(uint id : SV_VertexID) {\n"
        "    O o;\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    o.p = float4(uv * float2(2,-2) + float2(-1,1), 0, 1);\n"
        "    return o;\n"
        "}\n";

    char psSrc[512];
    snprintf(psSrc, sizeof(psSrc),
        "Texture2DMS<float4> src : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "    int2 c = int2(pos.xy);\n"
        "    float4 sum = (float4)0;\n"
        "    [unroll] for (uint i = 0; i < %u; i++)\n"
        "        sum += src.Load(c, i);\n"
        "    return sum / %u.0;\n"
        "}\n", samples, samples);

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;

    if (!sResolveVS)
    {
        HRESULT hr = D3DCompile(vsSrc, strlen(vsSrc), "DeferredResolveVS", nullptr, nullptr,
                                "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &blob, &errors);
        if (FAILED(hr))
        {
            if (errors) { Log("DeferredMSAA: VS compile error: %s",
                              (const char*)errors->GetBufferPointer()); errors->Release(); }
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
        sDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                     nullptr, &sResolveVS);
        blob->Release();
        if (!sResolveVS) return false;
    }

    if (!sResolvePS)
    {
        HRESULT hr = D3DCompile(psSrc, strlen(psSrc), "DeferredResolvePS", nullptr, nullptr,
                                "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &blob, &errors);
        if (FAILED(hr))
        {
            if (errors) { Log("DeferredMSAA: PS compile error: %s",
                              (const char*)errors->GetBufferPointer()); errors->Release(); }
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
        sDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                    nullptr, &sResolvePS);
        blob->Release();
        if (!sResolvePS) return false;
    }

    Log("DeferredMSAA: compiled resolve shaders (%ux)", samples);
    return true;
}

static bool EnsureBlendState()
{
    if (sBlendState) return true;
    if (!sDevice) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(sDevice->CreateBlendState(&bd, &sBlendState));
}

static bool EnsureLightingRT(UINT w, UINT h, uint32_t samples)
{
    if (sLightingMSAATex &&
        sLightingWidth == w && sLightingHeight == h && sLightingSamples == samples)
        return true;

    ReleaseLightingRT();

    UINT qualityLevels = 0;
    sDevice->CheckMultisampleQualityLevels(MSAA_RT_FORMAT, samples, &qualityLevels);
    if (qualityLevels == 0)
    {
        Log("DeferredMSAA: %ux MSAA not supported for RGBA16F", samples);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = MSAA_RT_FORMAT;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(sDevice->CreateTexture2D(&desc, nullptr, &sLightingMSAATex)))
    {
        Log("DeferredMSAA: failed to create MSAA lighting texture");
        return false;
    }
    if (FAILED(sDevice->CreateRenderTargetView(sLightingMSAATex, nullptr, &sLightingMSAARTV)))
    {
        sLightingMSAATex->Release(); sLightingMSAATex = nullptr;
        Log("DeferredMSAA: failed to create MSAA lighting RTV");
        return false;
    }
    if (FAILED(sDevice->CreateShaderResourceView(sLightingMSAATex, nullptr, &sLightingMSAASRV)))
    {
        sLightingMSAARTV->Release(); sLightingMSAARTV = nullptr;
        sLightingMSAATex->Release(); sLightingMSAATex = nullptr;
        Log("DeferredMSAA: failed to create MSAA lighting SRV");
        return false;
    }

    if (!CompileResolveShaders(samples))
    {
        sLightingMSAASRV->Release(); sLightingMSAASRV = nullptr;
        sLightingMSAARTV->Release(); sLightingMSAARTV = nullptr;
        sLightingMSAATex->Release(); sLightingMSAATex = nullptr;
        Log("DeferredMSAA: failed to compile resolve shaders");
        return false;
    }

    sLightingWidth = w;
    sLightingHeight = h;
    sLightingSamples = samples;
    Log("DeferredMSAA: created %ux MSAA lighting RT (%ux%u, RGBA16F)", samples, w, h);
    return true;
}

void SetDevice(ID3D11Device* device)
{
    if (sDevice != device)
    {
        ReleaseLightingRT();
        ReleaseCB();
        if (sBlendState) { sBlendState->Release(); sBlendState = nullptr; }
    }
    sDevice = device;
}

void BindForLighting(ID3D11DeviceContext* ctx)
{
    uint32_t sampleCount = MSAARedirect::GetSampleCount();
    if (sampleCount < 2 || !sDevice) return;
    if (!EnsureCB()) return;

    ID3D11ShaderResourceView* msaa0 = MSAARedirect::GetColorSRV(0);
    ID3D11ShaderResourceView* msaa1 = MSAARedirect::GetColorSRV(1);
    ID3D11ShaderResourceView* msaa2 = MSAARedirect::GetColorSRV(2);
    if (!msaa0 || !msaa1 || !msaa2) return;

    uint32_t shaderSamples = sPerSampleActive ? sampleCount : 0;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(sCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        MSAACBLayout* cb = (MSAACBLayout*)mapped.pData;
        cb->sampleCount = shaderSamples;
        cb->edgeThreshold = 0;
        cb->debugMode = (float)sDebugMode;
        cb->pad = 0;
        ctx->Unmap(sCB, 0);
    }

    ID3D11ShaderResourceView* srvs[3] = { msaa0, msaa1, msaa2 };
    ctx->PSSetShaderResources(10, 3, srvs);
    ctx->PSSetConstantBuffers(3, 1, &sCB);

    static bool sFirstBind = true;
    if (sFirstBind)
    {
        Log("DeferredMSAA: bound MSAA SRVs at t10-t12, CB at b3 (%ux, perSample=%d)",
            sampleCount, sPerSampleActive ? 1 : 0);
        sFirstBind = false;
    }
}

void UnbindAfterLighting(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* nullSRVs[3] = {};
    ctx->PSSetShaderResources(10, 3, nullSRVs);
}

bool BeginPerSampleDraw(ID3D11DeviceContext* ctx)
{
    uint32_t samples = MSAARedirect::GetSampleCount();
    if (samples < 2 || !sDevice || sDebugMode > 0) return false;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv) { if (dsv) dsv->Release(); return false; }

    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (!res) { rtv->Release(); if (dsv) dsv->Release(); return false; }

    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (!tex) { rtv->Release(); if (dsv) dsv->Release(); return false; }

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    tex->Release();

    if (!EnsureLightingRT(desc.Width, desc.Height, samples))
    {
        rtv->Release();
        if (dsv) dsv->Release();
        return false;
    }

    sSavedRTV = rtv;
    sSavedDSV = dsv;

    const float clearColor[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(sLightingMSAARTV, clearColor);
    ctx->OMSetRenderTargets(1, &sLightingMSAARTV, nullptr);

    sPerSampleActive = true;

    static bool sFirstBegin = true;
    if (sFirstBegin)
    {
        Log("DeferredMSAA: per-sample lighting active (%ux%u %ux)",
            desc.Width, desc.Height, samples);
        sFirstBegin = false;
    }

    return true;
}

void EndPerSampleDraw(ID3D11DeviceContext* ctx)
{
    if (!sPerSampleActive) return;
    sPerSampleActive = false;

    if (sSavedRTV && sLightingMSAASRV && sResolveVS && sResolvePS && EnsureBlendState())
    {
        sStateBlock.Capture(ctx);

        ctx->OMSetRenderTargets(1, &sSavedRTV, nullptr);

        float bf[4] = { 1, 1, 1, 1 };
        ctx->OMSetBlendState(sBlendState, bf, 0xFFFFFFFF);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)sLightingWidth;
        vp.Height = (float)sLightingHeight;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(nullptr);

        ctx->PSSetShaderResources(0, 1, &sLightingMSAASRV);
        ctx->VSSetShader(sResolveVS, nullptr, 0);
        ctx->PSSetShader(sResolvePS, nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        sStateBlock.Restore(ctx);
    }

    // Always restore the original render targets
    ctx->OMSetRenderTargets(1, &sSavedRTV, sSavedDSV);

    if (sSavedRTV) { sSavedRTV->Release(); sSavedRTV = nullptr; }
    if (sSavedDSV) { sSavedDSV->Release(); sSavedDSV = nullptr; }
}

void SetDebugMode(int mode) { sDebugMode = mode; }
int GetDebugMode() { return sDebugMode; }
uint32_t GetSampleCount() { return MSAARedirect::GetSampleCount(); }

void Shutdown()
{
    ReleaseLightingRT();
    ReleaseCB();
    if (sBlendState) { sBlendState->Release(); sBlendState = nullptr; }
    if (sSavedRTV) { sSavedRTV->Release(); sSavedRTV = nullptr; }
    if (sSavedDSV) { sSavedDSV->Release(); sSavedDSV = nullptr; }
    sPerSampleActive = false;
    sDevice = nullptr;
}

} // namespace DeferredMSAA
