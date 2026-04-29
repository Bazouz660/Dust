#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

namespace ShaderPatch
{
    typedef HRESULT(WINAPI* PFN_D3DCompileHook)(
        LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
        const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
        LPCSTR pEntrypoint, LPCSTR pTarget,
        UINT Flags1, UINT Flags2,
        ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

    extern PFN_D3DCompileHook oD3DCompile;

    HRESULT WINAPI HookedD3DCompile(
        LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
        const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
        LPCSTR pEntrypoint, LPCSTR pTarget,
        UINT Flags1, UINT Flags2,
        ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);
}

// Safe defaults bound at b2/b3/b4 (CB) and s8/s9/s13 (SRV/sampler) before the
// vanilla deferred lighting pass runs, so the patched shader doesn't sample
// undefined CBs/textures when no Dust effect plugin is active. Implementation
// in ShaderPatch.cpp; called from D3D11Hook.
namespace DustDeferredDefaults
{
    void BindBeforeDeferred(ID3D11DeviceContext* ctx, ID3D11Device* device);
    void Shutdown();
}
