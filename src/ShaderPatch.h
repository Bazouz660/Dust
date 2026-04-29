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
