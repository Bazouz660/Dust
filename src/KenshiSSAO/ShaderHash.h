#pragma once

#include <d3d11.h>
#include <cstring>

// DXBC container format:
//   Bytes 0-3:   Magic "DXBC" (0x44584243)
//   Bytes 4-19:  128-bit checksum (4 x uint32) — stable shader identity
//   Bytes 20-23: Version (1)
//   Bytes 24-27: Total size
//   Bytes 28+:   Chunk count + offsets
//
// Source: timjones.io/blog/archive/2015/09/02/parsing-direct3d-shader-bytecode
//         llvm.org/docs/DirectX/DXContainer.html

struct DXBCHash
{
    unsigned int hash[4];

    bool operator==(const DXBCHash& other) const
    {
        return hash[0] == other.hash[0] && hash[1] == other.hash[1] &&
               hash[2] == other.hash[2] && hash[3] == other.hash[3];
    }

    bool operator!=(const DXBCHash& other) const { return !(*this == other); }

    bool IsZero() const
    {
        return hash[0] == 0 && hash[1] == 0 && hash[2] == 0 && hash[3] == 0;
    }

    // Match against first uint32 only (for quick checks against known hashes)
    bool StartsWith(unsigned int prefix) const { return hash[0] == prefix; }
};

// GUID for storing DXBCHash on shader objects via SetPrivateData/GetPrivateData.
// {7E3A1F2B-D4C5-4A8E-B6F9-0123456789AB}
static const GUID GUID_DXBCHash = {
    0x7e3a1f2b, 0xd4c5, 0x4a8e, { 0xb6, 0xf9, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab }
};

// Extract the 128-bit DXBC hash from compiled shader bytecode.
// Returns true if the bytecode is valid DXBC, false otherwise.
inline bool ExtractDXBCHash(const void* bytecode, SIZE_T bytecodeLength, DXBCHash& outHash)
{
    if (!bytecode || bytecodeLength < 20)
        return false;

    const unsigned char* data = static_cast<const unsigned char*>(bytecode);

    // Verify magic "DXBC"
    if (data[0] != 'D' || data[1] != 'X' || data[2] != 'B' || data[3] != 'C')
        return false;

    memcpy(&outHash.hash[0], data + 4, 16);
    return true;
}

// Store hash on an ID3D11PixelShader via SetPrivateData.
inline void StoreHashOnShader(ID3D11PixelShader* shader, const DXBCHash& hash)
{
    if (shader)
        shader->SetPrivateData(GUID_DXBCHash, sizeof(DXBCHash), &hash);
}

// Retrieve hash from an ID3D11PixelShader via GetPrivateData.
// Returns true if hash was found, false otherwise.
inline bool GetHashFromShader(ID3D11PixelShader* shader, DXBCHash& outHash)
{
    if (!shader)
        return false;

    UINT size = sizeof(DXBCHash);
    HRESULT hr = shader->GetPrivateData(GUID_DXBCHash, &size, &outHash);
    return SUCCEEDED(hr) && size == sizeof(DXBCHash);
}
