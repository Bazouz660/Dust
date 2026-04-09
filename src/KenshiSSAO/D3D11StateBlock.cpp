#include "D3D11StateBlock.h"
#include <cstring>

D3D11StateBlock::D3D11StateBlock()
    : captured(false)
{
    memset(this, 0, sizeof(*this));
}

D3D11StateBlock::~D3D11StateBlock()
{
    ReleaseAll();
}

void D3D11StateBlock::Capture(ID3D11DeviceContext* ctx)
{
    ReleaseAll();

    // IA
    ctx->IAGetInputLayout(&iaLayout);
    ctx->IAGetPrimitiveTopology(&iaTopology);

    // VS
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ctx->VSGetConstantBuffers(0, 1, vsCBs);

    // PS
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 4, psSRVs);
    ctx->PSGetSamplers(0, 2, psSamplers);
    ctx->PSGetConstantBuffers(0, 1, psCBs);

    // OM
    ctx->OMGetRenderTargets(4, omRTVs, &omDSV);
    ctx->OMGetBlendState(&omBlendState, omBlendFactor, &omSampleMask);
    ctx->OMGetDepthStencilState(&omDepthStencilState, &omStencilRef);

    // RS
    rsNumViewports = 1;
    ctx->RSGetViewports(&rsNumViewports, rsViewports);
    ctx->RSGetState(&rsState);

    captured = true;
}

void D3D11StateBlock::Restore(ID3D11DeviceContext* ctx)
{
    if (!captured)
        return;

    // IA
    ctx->IASetInputLayout(iaLayout);
    ctx->IASetPrimitiveTopology(iaTopology);

    // VS
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, vsCBs);

    // PS
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 4, psSRVs);
    ctx->PSSetSamplers(0, 2, psSamplers);
    ctx->PSSetConstantBuffers(0, 1, psCBs);

    // OM
    ctx->OMSetRenderTargets(4, omRTVs, omDSV);
    ctx->OMSetBlendState(omBlendState, omBlendFactor, omSampleMask);
    ctx->OMSetDepthStencilState(omDepthStencilState, omStencilRef);

    // RS
    ctx->RSSetViewports(rsNumViewports, rsViewports);
    ctx->RSSetState(rsState);

    ReleaseAll();
}

void D3D11StateBlock::ReleaseAll()
{
    if (!captured)
        return;

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

    SAFE_RELEASE(iaLayout);
    SAFE_RELEASE(vs);
    SAFE_RELEASE(vsCBs[0]);
    SAFE_RELEASE(ps);
    for (int i = 0; i < 4; i++) SAFE_RELEASE(psSRVs[i]);
    for (int i = 0; i < 2; i++) SAFE_RELEASE(psSamplers[i]);
    SAFE_RELEASE(psCBs[0]);
    for (int i = 0; i < 4; i++) SAFE_RELEASE(omRTVs[i]);
    SAFE_RELEASE(omDSV);
    SAFE_RELEASE(omBlendState);
    SAFE_RELEASE(omDepthStencilState);
    SAFE_RELEASE(rsState);

#undef SAFE_RELEASE

    captured = false;
}
