#pragma once

#include <d3d11.h>
#include <cstdint>

// How the VS transforms vertices to clip space.
// Determined by which matrix parameter the shader declares.
enum class VSTransformType : uint8_t
{
    UNKNOWN = 0,    // Could not classify — not a GBuffer VS, or reflection failed
    STATIC,         // Uses worldViewProjMatrix (combined W*V*P) — objects, terrain, foliage, triplanar, distant_town
    SKINNED         // Uses viewProjectionMatrix (V*P only) — skin, character, creature (bones handle W)
};

struct VSConstantBufferInfo
{
    VSTransformType transformType = VSTransformType::UNKNOWN;

    // Byte offset of the clip-space transform matrix in the VS constant buffer.
    //   STATIC:  offset of worldViewProjMatrix (float4x4, 64 bytes)
    //   SKINNED: offset of viewProjectionMatrix (float4x4, 64 bytes)
    uint32_t clipMatrixOffset = 0;
    uint32_t clipMatrixSize   = 0;

    // Byte offset of worldMatrix (float4x4, 64 bytes). Zero if not present.
    uint32_t worldMatrixOffset = 0;
    uint32_t worldMatrixSize   = 0;

    // Total size of the constant buffer containing the matrices.
    uint32_t cbTotalSize = 0;

    // Which cbuffer register slot the matrices live in (b0, b1, ...).
    uint32_t cbSlot = 0;
};

namespace ShaderMetadata
{
    // Called from HookedCreateVertexShader after the VS is created.
    // Runs D3DReflect on the bytecode to extract the constant buffer layout.
    // Safe to call for any VS — non-GBuffer shaders are stored as UNKNOWN.
    void OnVertexShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                               ID3D11VertexShader* vs);

    // Look up metadata for a VS. Returns nullptr if the VS was never seen.
    const VSConstantBufferInfo* GetVSInfo(ID3D11VertexShader* vs);

    // Release all stored metadata.
    void Shutdown();

    // Diagnostics
    uint32_t GetTrackedCount();
    uint32_t GetClassifiedCount();
}
