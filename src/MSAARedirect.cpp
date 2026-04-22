#include "MSAARedirect.h"
#include "GeometryCapture.h"
#include "D3D11StateBlock.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <cstdio>

namespace MSAARedirect
{

static ID3D11Device* sDevice = nullptr;
static UINT sWidth = 0, sHeight = 0;
static uint32_t sRequestedSamples = 0;
static uint32_t sActiveSamples = 0;
static bool sResourcesValid = false;
static bool sRedirectedThisPass = false;

static ID3D11Texture2D*          sMSAARTs[3]   = {};
static ID3D11RenderTargetView*   sMSAARTVs[3]  = {};
static ID3D11Texture2D*          sMSAADepthTex  = nullptr;
static ID3D11DepthStencilView*   sMSAADSV       = nullptr;
static ID3D11ShaderResourceView* sMSAADepthSRV  = nullptr;

static ID3D11RenderTargetView*   sOrigRTVs[3]   = {};
static ID3D11DepthStencilView*   sOrigDSV       = nullptr;

static ID3D11VertexShader*       sResolveVS     = nullptr;
static ID3D11PixelShader*        sResolvePS     = nullptr;
static ID3D11DepthStencilState*  sDepthWriteAll = nullptr;

static D3D11StateBlock sStateBlock;

static const DXGI_FORMAT (&COLOR_FORMATS)[3] = GBUFFER_COLOR_FORMATS;

static void ReleaseResources()
{
    for (int i = 0; i < 3; i++)
    {
        if (sMSAARTVs[i]) { sMSAARTVs[i]->Release(); sMSAARTVs[i] = nullptr; }
        if (sMSAARTs[i])  { sMSAARTs[i]->Release();  sMSAARTs[i] = nullptr; }
    }
    if (sMSAADepthSRV) { sMSAADepthSRV->Release(); sMSAADepthSRV = nullptr; }
    if (sMSAADSV)      { sMSAADSV->Release();      sMSAADSV = nullptr; }
    if (sMSAADepthTex) { sMSAADepthTex->Release();  sMSAADepthTex = nullptr; }
    sResourcesValid = false;
}

static void ReleaseOriginals()
{
    for (int i = 0; i < 3; i++)
        if (sOrigRTVs[i]) { sOrigRTVs[i]->Release(); sOrigRTVs[i] = nullptr; }
    if (sOrigDSV) { sOrigDSV->Release(); sOrigDSV = nullptr; }
    sRedirectedThisPass = false;
}

static bool CompileResolveShaders()
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
        "Texture2DMS<float> t : register(t0);\n"
        "float main(float4 p : SV_Position) : SV_Depth {\n"
        "    int2 c = int2(p.xy);\n"
        "    float d = 1.0;\n"
        "    [unroll] for (uint i = 0; i < %u; i++)\n"
        "        d = min(d, t.Load(c, i).x);\n"
        "    return d;\n"
        "}\n", sActiveSamples);

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;

    if (!sResolveVS)
    {
        HRESULT hr = D3DCompile(vsSrc, strlen(vsSrc), "MSAAResolveVS", nullptr, nullptr,
                                "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &blob, &errors);
        if (FAILED(hr))
        {
            if (errors) { Log("MSAARedirect: VS compile error: %s",
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
        HRESULT hr = D3DCompile(psSrc, strlen(psSrc), "MSAAResolvePS", nullptr, nullptr,
                                "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &blob, &errors);
        if (FAILED(hr))
        {
            if (errors) { Log("MSAARedirect: PS compile error: %s",
                              (const char*)errors->GetBufferPointer()); errors->Release(); }
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
        sDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                    nullptr, &sResolvePS);
        blob->Release();
        if (!sResolvePS) return false;
    }

    return true;
}

static bool CreateResources()
{
    if (!sDevice || sWidth == 0 || sHeight == 0 || sRequestedSamples < 2)
        return false;

    ReleaseResources();

    UINT qualityLevels = 0;
    for (int i = 0; i < 3; i++)
    {
        sDevice->CheckMultisampleQualityLevels(COLOR_FORMATS[i], sRequestedSamples, &qualityLevels);
        if (qualityLevels == 0)
        {
            Log("MSAARedirect: %ux MSAA not supported for format %u",
                sRequestedSamples, COLOR_FORMATS[i]);
            return false;
        }
    }

    sDevice->CheckMultisampleQualityLevels(DXGI_FORMAT_R24G8_TYPELESS,
                                            sRequestedSamples, &qualityLevels);
    if (qualityLevels == 0)
    {
        Log("MSAARedirect: %ux MSAA not supported for depth format", sRequestedSamples);
        return false;
    }

    sActiveSamples = sRequestedSamples;

    for (int i = 0; i < 3; i++)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = sWidth;
        desc.Height = sHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = COLOR_FORMATS[i];
        desc.SampleDesc.Count = sActiveSamples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        if (FAILED(sDevice->CreateTexture2D(&desc, nullptr, &sMSAARTs[i])))
            { ReleaseResources(); return false; }
        if (FAILED(sDevice->CreateRenderTargetView(sMSAARTs[i], nullptr, &sMSAARTVs[i])))
            { ReleaseResources(); return false; }
    }

    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = sWidth;
        desc.Height = sHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.SampleDesc.Count = sActiveSamples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(sDevice->CreateTexture2D(&desc, nullptr, &sMSAADepthTex)))
            { ReleaseResources(); return false; }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        if (FAILED(sDevice->CreateDepthStencilView(sMSAADepthTex, &dsvDesc, &sMSAADSV)))
            { ReleaseResources(); return false; }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        if (FAILED(sDevice->CreateShaderResourceView(sMSAADepthTex, &srvDesc, &sMSAADepthSRV)))
            { ReleaseResources(); return false; }
    }

    if (!sDepthWriteAll)
    {
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable = TRUE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        sDevice->CreateDepthStencilState(&dsDesc, &sDepthWriteAll);
    }

    if (!CompileResolveShaders())
    {
        Log("MSAARedirect: failed to compile resolve shaders");
        ReleaseResources();
        return false;
    }

    sResourcesValid = true;
    Log("MSAARedirect: created %ux MSAA targets (%ux%u)", sActiveSamples, sWidth, sHeight);
    return true;
}

void SetEnabled(uint32_t sampleCount)
{
    if (sampleCount == sRequestedSamples)
        return;

    sRequestedSamples = sampleCount;

    if (sampleCount < 2)
    {
        ReleaseResources();
        sActiveSamples = 0;
        Log("MSAARedirect: disabled");
        return;
    }

    // Invalidate — will recreate on next GBuffer enter
    if (sResourcesValid && sActiveSamples != sampleCount)
    {
        if (sResolvePS) { sResolvePS->Release(); sResolvePS = nullptr; }
        ReleaseResources();
    }

    Log("MSAARedirect: requested %ux (will activate on next GBuffer pass)", sampleCount);
}

uint32_t GetSampleCount()
{
    return sRequestedSamples;
}

bool IsActive()
{
    return sRedirectedThisPass;
}

void SetDevice(ID3D11Device* device)
{
    if (sDevice != device)
    {
        ReleaseResources();
        ReleaseOriginals();
    }
    sDevice = device;
}

void SetResolution(UINT width, UINT height)
{
    if (width != sWidth || height != sHeight)
    {
        sWidth = width;
        sHeight = height;
        if (sResourcesValid)
            ReleaseResources();
    }
}

bool OnGBufferEnter(ID3D11RenderTargetView* const* origRTVs,
                    ID3D11DepthStencilView* origDSV,
                    ID3D11RenderTargetView** outMSAARTVs,
                    ID3D11DepthStencilView** outMSAADSV)
{
    if (sRequestedSamples < 2 || !sDevice)
        return false;

    if (!sResourcesValid)
    {
        if (!CreateResources())
        {
            sRequestedSamples = 0;
            return false;
        }
    }

    ReleaseOriginals();
    for (int i = 0; i < 3; i++)
    {
        sOrigRTVs[i] = origRTVs[i];
        if (sOrigRTVs[i]) sOrigRTVs[i]->AddRef();
    }
    sOrigDSV = origDSV;
    if (sOrigDSV) sOrigDSV->AddRef();

    for (int i = 0; i < 3; i++)
        outMSAARTVs[i] = sMSAARTVs[i];
    *outMSAADSV = sMSAADSV;

    sRedirectedThisPass = true;
    return true;
}

void OnGBufferLeave(ID3D11DeviceContext* ctx)
{
    if (!sRedirectedThisPass || !sResourcesValid)
        return;

    // Resolve color RTs
    for (int i = 0; i < 3; i++)
    {
        if (!sMSAARTs[i] || !sOrigRTVs[i])
            continue;
        ID3D11Resource* origRes = nullptr;
        sOrigRTVs[i]->GetResource(&origRes);
        if (origRes)
        {
            ctx->ResolveSubresource(origRes, 0, sMSAARTs[i], 0, COLOR_FORMATS[i]);
            origRes->Release();
        }
    }

    // Resolve depth via custom shader (ResolveSubresource doesn't support depth)
    if (sMSAADepthSRV && sOrigDSV && sResolveVS && sResolvePS && sDepthWriteAll)
    {
        sStateBlock.Capture(ctx);

        ID3D11RenderTargetView* nullRTV = nullptr;
        ctx->OMSetRenderTargets(1, &nullRTV, sOrigDSV);
        ctx->OMSetDepthStencilState(sDepthWriteAll, 0);

        float blendFactor[4] = { 1, 1, 1, 1 };
        ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)sWidth;
        vp.Height = (float)sHeight;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(nullptr);

        ctx->PSSetShaderResources(0, 1, &sMSAADepthSRV);
        ctx->VSSetShader(sResolveVS, nullptr, 0);
        ctx->PSSetShader(sResolvePS, nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSRV);

        sStateBlock.Restore(ctx);
    }

    ReleaseOriginals();
}

void Shutdown()
{
    ReleaseResources();
    ReleaseOriginals();
    if (sResolveVS)     { sResolveVS->Release();     sResolveVS = nullptr; }
    if (sResolvePS)     { sResolvePS->Release();     sResolvePS = nullptr; }
    if (sDepthWriteAll) { sDepthWriteAll->Release(); sDepthWriteAll = nullptr; }
    sDevice = nullptr;
}

} // namespace MSAARedirect
