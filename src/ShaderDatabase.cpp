#include "ShaderDatabase.h"
#include "SurveyRecorder.h"
#include "DustLog.h"
#include <unordered_map>
#include <string>
#include <cstring>

namespace ShaderDatabase
{

struct ShaderEntry
{
    DustShaderCategory category;
    std::string sourceName;
};

static std::unordered_map<uint64_t, ShaderEntry> sShaders;

static DustShaderCategory ClassifySourceName(const char* name)
{
    if (!name || !name[0])
        return DUST_SHADER_UNKNOWN;

    if (strstr(name, "skin"))          return DUST_SHADER_SKIN;
    if (strstr(name, "character"))     return DUST_SHADER_SKIN;     // severed_limb_vs etc.
    if (strstr(name, "birds"))         return DUST_SHADER_FOLIAGE;  // viewProj-only, world built in VS
    if (strstr(name, "foliage"))       return DUST_SHADER_FOLIAGE;
    if (strstr(name, "terrain"))       return DUST_SHADER_TERRAIN;
    if (strstr(name, "triplanar"))     return DUST_SHADER_TRIPLANAR;
    if (strstr(name, "distant_town"))  return DUST_SHADER_DISTANT_TOWN;
    if (strstr(name, "mapfeature"))    return DUST_SHADER_OBJECTS;  // static world objects
    if (strstr(name, "objects"))       return DUST_SHADER_OBJECTS;
    if (strstr(name, "object"))        return DUST_SHADER_OBJECTS;

    return DUST_SHADER_UNKNOWN;
}

static void RegisterShader(uint64_t ptr)
{
    const ShaderSourceInfo* info = SurveyRecorder::GetShaderSource(ptr);
    if (!info)
        return;

    ShaderEntry entry;
    entry.sourceName = info->sourceName;
    entry.category = ClassifySourceName(info->sourceName.c_str());

    DustShaderCategory cat = entry.category;
    sShaders[ptr] = std::move(entry);

    if (cat != DUST_SHADER_UNKNOWN)
    {
        Log("ShaderDatabase: %p -> %s (category %u)",
            (void*)(uintptr_t)ptr, sShaders[ptr].sourceName.c_str(), (uint32_t)cat);
    }
}

void OnPixelShaderCreated(ID3D11PixelShader* ps)
{
    if (ps) RegisterShader((uint64_t)ps);
}

void OnVertexShaderCreated(ID3D11VertexShader* vs)
{
    if (vs) RegisterShader((uint64_t)vs);
}

DustShaderCategory GetPixelShaderCategory(ID3D11PixelShader* ps)
{
    auto it = sShaders.find((uint64_t)ps);
    return (it != sShaders.end()) ? it->second.category : DUST_SHADER_UNKNOWN;
}

DustShaderCategory GetVertexShaderCategory(ID3D11VertexShader* vs)
{
    auto it = sShaders.find((uint64_t)vs);
    return (it != sShaders.end()) ? it->second.category : DUST_SHADER_UNKNOWN;
}

const char* GetSourceName(uint64_t shaderPtr)
{
    auto it = sShaders.find(shaderPtr);
    return (it != sShaders.end()) ? it->second.sourceName.c_str() : nullptr;
}

void Shutdown()
{
    Log("ShaderDatabase: shutdown — %u shaders tracked", (uint32_t)sShaders.size());
    sShaders.clear();
}

} // namespace ShaderDatabase
