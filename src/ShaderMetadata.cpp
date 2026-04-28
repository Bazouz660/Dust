#include "ShaderMetadata.h"
#include "ShaderSourceCatalog.h"
#include "SurveyRecorder.h"
#include "DustLog.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <unordered_map>
#include <cstring>
#include <string>

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

// Strip the directory + extension from "deferred/skin.hlsl" -> "skin".
static std::string SourceBasename(const std::string& s)
{
    if (s.empty()) return s;
    auto slash = s.find_last_of("\\/");
    std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
    auto dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

// Use the catalog (built from HLSL sources) to determine the VS transform type
// and whether worldMatrix is present. Returns true on success and writes the
// classification into outType + outHasWorld.
static bool ClassifyFromCatalog(ID3D11VertexShader* vs,
                                VSTransformType& outType,
                                bool& outHasWorld)
{
    const ShaderSourceInfo* src = SurveyRecorder::GetShaderSource((uint64_t)vs);
    if (!src || src->sourceName.empty() || src->entryPoint.empty())
        return false;

    std::string base = SourceBasename(src->sourceName);
    const ShaderSourceCatalog::ShaderFile* file =
        ShaderSourceCatalog::GetFileByBasename(base);
    if (!file) return false;

    auto rv = ShaderSourceCatalog::ResolveVariant(file, src->entryPoint, src->defines);
    if (!rv.valid || rv.entry->stage != ShaderSourceCatalog::ShaderStage::Vertex)
        return false;

    outType = VSTransformType::UNKNOWN;
    outHasWorld = false;
    for (const auto* u : rv.activeUniforms)
    {
        for (int i = 0; i < CLIP_MATRIX_NAME_COUNT; ++i)
        {
            if (u->name == CLIP_MATRIX_NAMES[i].name)
            {
                outType = CLIP_MATRIX_NAMES[i].type;
                break;
            }
        }
        if (u->name == WORLD_MATRIX_NAME)
            outHasWorld = true;
    }
    return outType != VSTransformType::UNKNOWN;
}

// Walk the reflection's CBs and fill in the byte offsets/sizes for the matrix
// variables we care about. Caller has already classified by name (either via
// catalog or by reflection-name-walk fallback). The first matching matrix in
// any CB wins for clipMatrix; worldMatrix is optional.
static bool ReflectMatrixOffsets(const void* bytecode, SIZE_T bytecodeSize,
                                 VSConstantBufferInfo& info,
                                 char* outSigBuf, size_t sigBufSize)
{
    ID3D11ShaderReflection* reflector = nullptr;
    HRESULT hr = D3DReflect(bytecode, bytecodeSize,
                            IID_ID3D11ShaderReflection, (void**)&reflector);
    if (FAILED(hr) || !reflector) return false;

    D3D11_SHADER_DESC shaderDesc;
    if (FAILED(reflector->GetDesc(&shaderDesc)))
    {
        reflector->Release();
        return false;
    }

    bool foundClip = false;
    for (UINT cbIdx = 0; cbIdx < shaderDesc.ConstantBuffers && !foundClip; cbIdx++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = reflector->GetConstantBufferByIndex(cbIdx);
        if (!cb) continue;
        D3D11_SHADER_BUFFER_DESC cbDesc;
        if (FAILED(cb->GetDesc(&cbDesc))) continue;
        if (cbDesc.Type != D3D_CT_CBUFFER) continue;

        for (UINT varIdx = 0; varIdx < cbDesc.Variables; varIdx++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(varIdx);
            if (!var) continue;
            D3D11_SHADER_VARIABLE_DESC varDesc;
            if (FAILED(var->GetDesc(&varDesc))) continue;

            if (!foundClip)
            {
                for (int i = 0; i < CLIP_MATRIX_NAME_COUNT; i++)
                {
                    if (strcmp(varDesc.Name, CLIP_MATRIX_NAMES[i].name) == 0)
                    {
                        info.clipMatrixOffset = varDesc.StartOffset;
                        info.clipMatrixSize   = varDesc.Size;
                        // If the catalog hasn't already set transformType, do
                        // it from the reflected name (legacy fallback path).
                        if (info.transformType == VSTransformType::UNKNOWN)
                            info.transformType = CLIP_MATRIX_NAMES[i].type;
                        info.cbTotalSize      = cbDesc.Size;
                        FindCBSlot(reflector, shaderDesc, cbDesc.Name, info.cbSlot);
                        foundClip = true;
                        break;
                    }
                }
            }

            if (strcmp(varDesc.Name, WORLD_MATRIX_NAME) == 0)
            {
                info.worldMatrixOffset = varDesc.StartOffset;
                info.worldMatrixSize   = varDesc.Size;
            }
        }
    }

    // Capture the output signature for classified shaders.
    if (outSigBuf && sigBufSize > 0 && info.transformType != VSTransformType::UNKNOWN)
    {
        outSigBuf[0] = '\0';
        size_t off = 0;
        for (UINT i = 0; i < shaderDesc.OutputParameters; i++)
        {
            D3D11_SIGNATURE_PARAMETER_DESC sigDesc;
            if (FAILED(reflector->GetOutputParameterDesc(i, &sigDesc)))
                continue;
            const char* sem = sigDesc.SemanticName ? sigDesc.SemanticName : "(null)";
            int n = _snprintf_s(outSigBuf + off, sigBufSize - off, _TRUNCATE,
                                "%s%s%u(r%u m0x%X)",
                                (off ? ", " : ""),
                                sem, sigDesc.SemanticIndex,
                                sigDesc.Register, sigDesc.Mask);
            if (n > 0) off += (size_t)n;
            if (off >= sigBufSize - 32) break;
        }
    }

    reflector->Release();
    return foundClip;
}

void OnVertexShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                           ID3D11VertexShader* vs)
{
    if (!bytecode || bytecodeSize == 0 || !vs)
        return;

    VSConstantBufferInfo info;

    // --- Phase 1.E: catalog-first classification ---
    // Use the source-level catalog to determine transformType + worldMatrix
    // presence by name, then fall back to D3DReflect for byte offsets only.
    // The catalog handles define-conditional uniforms correctly without
    // needing the compiler's "did this matrix survive optimization" data.
    bool fromCatalog = false;
    {
        VSTransformType t = VSTransformType::UNKNOWN;
        bool hasWorld = false;
        if (ClassifyFromCatalog(vs, t, hasWorld))
        {
            info.transformType = t;
            fromCatalog = true;
            (void)hasWorld; // offsets pulled from reflection below; presence inferred there
        }
    }

    // Fill in byte offsets, CB slot, and total size from D3DReflect. If the
    // catalog couldn't classify (unknown shader), reflection's name walk also
    // serves as the legacy classification fallback.
    char sigBuf[1024] = {};
    bool foundClip = ReflectMatrixOffsets(bytecode, bytecodeSize, info, sigBuf, sizeof(sigBuf));

    sVSMap[vs] = info;

    if (info.transformType != VSTransformType::UNKNOWN)
    {
        const char* typeStr = (info.transformType == VSTransformType::SKINNED) ? "SKINNED" : "STATIC";
        const char* via     = fromCatalog ? "catalog" : "reflect";
        Log("ShaderMetadata: VS %p classified [%s] as %s — clip @%u (%uB), world @%u (%uB), cb slot %u size %u",
            vs, via, typeStr,
            info.clipMatrixOffset, info.clipMatrixSize,
            info.worldMatrixOffset, info.worldMatrixSize,
            info.cbSlot, info.cbTotalSize);
        Log("ShaderMetadata: VS %p output sig: %s", vs, sigBuf);

        // Mismatch detection: catalog said one thing, reflection said another.
        // Catalog is the design-of-record source; if reflection disagrees we
        // probably hit a parser bug. Logs but keeps catalog's classification.
        if (fromCatalog && foundClip)
        {
            // (No comparison failure path right now — both paths just write
            //  info.transformType, and the catalog wrote it first. Reflection
            //  only updated it when catalog left it UNKNOWN, so they cannot
            //  disagree by construction. Hook is reserved for future use.)
        }
    }
    (void)foundClip;
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
