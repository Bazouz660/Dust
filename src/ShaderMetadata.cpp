#include "ShaderMetadata.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <unordered_map>
#include <cstring>

namespace ShaderMetadata
{

static std::unordered_map<ID3D11VertexShader*, VSConstantBufferInfo> sVSMap;

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
    // farm_vs (foliage.hlsl), farm_shadow_vs (foliage.hlsl), birds_vs (birds.hlsl):
    // VS does its own world-space composition then projects with a camera-only
    // VP matrix. Treat as SKINNED for replay (clipMatrix == camera VP, no world
    // composition needed in passthrough mode).
    { "viewProjMatrix",       VSTransformType::SKINNED },
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
        }
    }

    // Dump output signature for classified shaders so we can verify a downstream GS
    // (e.g. for the DPM build pass) will link. Logged once per shader at creation time.
    char sigBuf[1024] = {};
    if (info.transformType != VSTransformType::UNKNOWN)
    {
        size_t off = 0;
        for (UINT i = 0; i < shaderDesc.OutputParameters; i++)
        {
            D3D11_SIGNATURE_PARAMETER_DESC sigDesc;
            if (FAILED(reflector->GetOutputParameterDesc(i, &sigDesc)))
                continue;
            const char* sem = sigDesc.SemanticName ? sigDesc.SemanticName : "(null)";
            int n = _snprintf_s(sigBuf + off, sizeof(sigBuf) - off, _TRUNCATE,
                                "%s%s%u(r%u m0x%X)",
                                (off ? ", " : ""),
                                sem, sigDesc.SemanticIndex,
                                sigDesc.Register, sigDesc.Mask);
            if (n > 0) off += (size_t)n;
            if (off >= sizeof(sigBuf) - 32) break;
        }
    }

    reflector->Release();

    sVSMap[vs] = info;

    if (info.transformType != VSTransformType::UNKNOWN)
    {
        const char* typeStr = (info.transformType == VSTransformType::SKINNED) ? "SKINNED" : "STATIC";
        Log("ShaderMetadata: VS %p classified as %s — clip @%u (%uB), world @%u (%uB), cb slot %u size %u",
            vs, typeStr,
            info.clipMatrixOffset, info.clipMatrixSize,
            info.worldMatrixOffset, info.worldMatrixSize,
            info.cbSlot, info.cbTotalSize);
        Log("ShaderMetadata: VS %p output sig: %s", vs, sigBuf);
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
