#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <string>

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

    // SKINNED: byte offset + count of worldMatrix3x4Array (bone palette, 48 bytes/bone).
    // The offset varies per shader (some declare uniforms before it), so it must be reflected.
    uint32_t boneArrayOffset = 0;
    uint32_t boneCount       = 0;

    // Total size of the constant buffer containing the matrices.
    uint32_t cbTotalSize = 0;

    // Which cbuffer register slot the matrices live in (b0, b1, ...).
    uint32_t cbSlot = 0;
};

// One element of a vertex declaration, captured from CreateInputLayout (deep copy: the
// D3D11_INPUT_ELEMENT_DESC.SemanticName pointer is caller-owned and dangles after the call).
struct InputElement
{
    std::string                semantic;
    uint32_t                   semanticIndex;
    DXGI_FORMAT                format;
    uint32_t                   inputSlot;
    uint32_t                   alignedByteOffset;
    D3D11_INPUT_CLASSIFICATION slotClass;
    uint32_t                   instanceStepRate;
};

namespace ShaderMetadata
{
    // Record the real vertex declaration for a created input layout (ground-truth slot/
    // offset/format per semantic — the only place OGRE's mesh vertex format is visible).
    void OnInputLayoutCreated(ID3D11InputLayout* layout,
                              const D3D11_INPUT_ELEMENT_DESC* descs, UINT count);
    // Look up the elements for a layout. Returns nullptr if never seen.
    const std::vector<InputElement>* GetInputLayoutElements(ID3D11InputLayout* layout);

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
