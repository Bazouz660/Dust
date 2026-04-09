#pragma once

#include <d3d11.h>

// Saves and restores all D3D11 pipeline state that our AO injection touches.
// Every Get call AddRefs COM objects — Release is called on restore/destruction.
class D3D11StateBlock
{
public:
    D3D11StateBlock();
    ~D3D11StateBlock();

    void Capture(ID3D11DeviceContext* ctx);
    void Restore(ID3D11DeviceContext* ctx);

private:
    void ReleaseAll();

    // IA
    ID3D11InputLayout* iaLayout;
    D3D11_PRIMITIVE_TOPOLOGY iaTopology;

    // VS
    ID3D11VertexShader* vs;
    ID3D11Buffer* vsCBs[1];

    // PS
    ID3D11PixelShader* ps;
    ID3D11ShaderResourceView* psSRVs[4];
    ID3D11SamplerState* psSamplers[2];
    ID3D11Buffer* psCBs[1];

    // OM
    ID3D11RenderTargetView* omRTVs[4];
    ID3D11DepthStencilView* omDSV;
    ID3D11BlendState* omBlendState;
    float omBlendFactor[4];
    UINT omSampleMask;
    ID3D11DepthStencilState* omDepthStencilState;
    UINT omStencilRef;

    // RS
    ID3D11RasterizerState* rsState;
    D3D11_VIEWPORT rsViewports[1];
    UINT rsNumViewports;

    bool captured;
};
