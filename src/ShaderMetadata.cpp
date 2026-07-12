#include "ShaderMetadata.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <unordered_map>
#include <cstring>

namespace ShaderMetadata
{

static std::unordered_map<ID3D11VertexShader*, VSConstantBufferInfo> sVSMap;
static std::unordered_map<ID3D11InputLayout*, std::vector<InputElement>> sLayoutMap;

void OnInputLayoutCreated(ID3D11InputLayout* layout,
                          const D3D11_INPUT_ELEMENT_DESC* descs, UINT count)
{
    if (!layout || !descs || count == 0) return;
    std::vector<InputElement> elems;
    elems.reserve(count);
    for (UINT i = 0; i < count; i++)
    {
        InputElement e;
        e.semantic          = descs[i].SemanticName ? descs[i].SemanticName : "";
        e.semanticIndex     = descs[i].SemanticIndex;
        e.format            = descs[i].Format;
        e.inputSlot         = descs[i].InputSlot;
        e.alignedByteOffset = descs[i].AlignedByteOffset;
        e.slotClass         = descs[i].InputSlotClass;
        e.instanceStepRate  = descs[i].InstanceDataStepRate;
        elems.push_back(std::move(e));
    }
    sLayoutMap[layout] = std::move(elems);
}

const std::vector<InputElement>* GetInputLayoutElements(ID3D11InputLayout* layout)
{
    auto it = sLayoutMap.find(layout);
    return (it != sLayoutMap.end()) ? &it->second : nullptr;
}

// Clip-space transform matrix names, verified from every Kenshi GBuffer VS source:
//   objects.hlsl    -> worldViewProjMatrix   (param_named_auto worldviewproj_matrix)
//   terrain.hlsl    -> worldViewProjMatrix   (param_named_auto worldviewproj_matrix)
//   triplanar.hlsl  -> worldViewProjMatrix   (param_named_auto worldviewproj_matrix)
//   distant_town.hlsl -> worldViewProjMatrix (param_named_auto worldviewproj_matrix)
//   foliage.hlsl    -> worldViewProj         (param_named_auto worldviewproj_matrix)
//   skin.hlsl       -> viewProjectionMatrix  (param_named_auto viewproj_matrix)
//
// The first five bind OGRE's worldviewproj_matrix (World*View*Proj, per-draw).
// skin.hlsl binds viewproj_matrix (View*Proj only — bone matrices handle the World transform).

struct ClipMatrixName
{
    const char* name;
    VSTransformType type;
};

static const ClipMatrixName CLIP_MATRIX_NAMES[] = {
    { "worldViewProjMatrix",  VSTransformType::STATIC  },
    { "worldViewProj",        VSTransformType::STATIC  },
    { "viewProjectionMatrix", VSTransformType::SKINNED },
};
static const int CLIP_MATRIX_NAME_COUNT = sizeof(CLIP_MATRIX_NAMES) / sizeof(CLIP_MATRIX_NAMES[0]);

static const char* WORLD_MATRIX_NAME = "worldMatrix";

static bool FindCBSlot(ID3D11ShaderReflection* reflector, const D3D11_SHADER_DESC& shaderDesc,
                       const char* cbName, uint32_t& outSlot)
{
    for (UINT i = 0; i < shaderDesc.BoundResources; i++)
    {
        D3D11_SHADER_INPUT_BIND_DESC bindDesc;
        if (SUCCEEDED(reflector->GetResourceBindingDesc(i, &bindDesc)) &&
            bindDesc.Type == D3D_SIT_CBUFFER &&
            strcmp(bindDesc.Name, cbName) == 0)
        {
            outSlot = bindDesc.BindPoint;
            return true;
        }
    }
    return false;
}

void OnVertexShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                           ID3D11VertexShader* vs)
{
    if (!bytecode || bytecodeSize == 0 || !vs)
        return;

    // Overwrite if we've seen this pointer before (handles shader recreation)
    ID3D11ShaderReflection* reflector = nullptr;
    HRESULT hr = D3DReflect(bytecode, bytecodeSize,
                            IID_ID3D11ShaderReflection, (void**)&reflector);
    if (FAILED(hr) || !reflector)
        return;

    D3D11_SHADER_DESC shaderDesc;
    if (FAILED(reflector->GetDesc(&shaderDesc)))
    {
        reflector->Release();
        return;
    }
    UINT verMajor = D3D11_SHVER_GET_MAJOR(shaderDesc.Version);
    UINT verMinor = D3D11_SHVER_GET_MINOR(shaderDesc.Version);

    VSConstantBufferInfo info;
    bool foundClip = false;

    for (UINT cbIdx = 0; cbIdx < shaderDesc.ConstantBuffers && !foundClip; cbIdx++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = reflector->GetConstantBufferByIndex(cbIdx);
        if (!cb) continue;

        D3D11_SHADER_BUFFER_DESC cbDesc;
        if (FAILED(cb->GetDesc(&cbDesc))) continue;

        // Only inspect cbuffers (not tbuffers or other types)
        if (cbDesc.Type != D3D_CT_CBUFFER) continue;

        for (UINT varIdx = 0; varIdx < cbDesc.Variables; varIdx++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(varIdx);
            if (!var) continue;

            D3D11_SHADER_VARIABLE_DESC varDesc;
            if (FAILED(var->GetDesc(&varDesc))) continue;

            // Check for clip-space transform matrix
            if (!foundClip)
            {
                for (int i = 0; i < CLIP_MATRIX_NAME_COUNT; i++)
                {
                    if (strcmp(varDesc.Name, CLIP_MATRIX_NAMES[i].name) == 0)
                    {
                        info.clipMatrixOffset = varDesc.StartOffset;
                        info.clipMatrixSize   = varDesc.Size;
                        info.transformType    = CLIP_MATRIX_NAMES[i].type;
                        info.cbTotalSize      = cbDesc.Size;
                        FindCBSlot(reflector, shaderDesc, cbDesc.Name, info.cbSlot);
                        foundClip = true;
                        break;
                    }
                }
            }

            // Check for world matrix
            if (strcmp(varDesc.Name, WORLD_MATRIX_NAME) == 0)
            {
                info.worldMatrixOffset = varDesc.StartOffset;
                info.worldMatrixSize   = varDesc.Size;
            }

            // Skinned bone palette (worldMatrix3x4Array[BONES]) — offset varies per shader
            // (some declare uniforms before it), so the MV pass must read it from here, not 0.
            if (strcmp(varDesc.Name, "worldMatrix3x4Array") == 0)
            {
                info.boneArrayOffset = varDesc.StartOffset;
                info.boneCount       = varDesc.Size / 48;   // each float3x4 bone = 48 bytes (row-major)
            }
        }
    }

    reflector->Release();

    sVSMap[vs] = info;

    if (info.transformType != VSTransformType::UNKNOWN)
    {
        const char* typeStr = (info.transformType == VSTransformType::SKINNED) ? "SKINNED" : "STATIC";
        Log("ShaderMetadata: VS %p classified as %s sm=%u_%u — clip @%u (%uB), world @%u (%uB), cb slot %u size %u",
            vs, typeStr, verMajor, verMinor,
            info.clipMatrixOffset, info.clipMatrixSize,
            info.worldMatrixOffset, info.worldMatrixSize,
            info.cbSlot, info.cbTotalSize);
    }
}

const VSConstantBufferInfo* GetVSInfo(ID3D11VertexShader* vs)
{
    auto it = sVSMap.find(vs);
    return (it != sVSMap.end()) ? &it->second : nullptr;
}

void Shutdown()
{
    uint32_t total = (uint32_t)sVSMap.size();
    uint32_t classified = GetClassifiedCount();
    Log("ShaderMetadata: shutdown — %u VS tracked, %u classified as GBuffer", total, classified);
    sVSMap.clear();
}

uint32_t GetTrackedCount()
{
    return (uint32_t)sVSMap.size();
}

uint32_t GetClassifiedCount()
{
    uint32_t count = 0;
    for (const auto& pair : sVSMap)
    {
        if (pair.second.transformType != VSTransformType::UNKNOWN)
            count++;
    }
    return count;
}

} // namespace ShaderMetadata
