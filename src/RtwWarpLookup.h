#pragma once

#include <d3dcompiler.h>
#include <map>
#include <regex>
#include <string>

// The RTWSM warp map has 513 knots, and the builder places knot i at u = i / 512
// (st_loc = lerp(-1, 1, i / 512)). Every lookup samples it like an ordinary texture,
// which puts knot i at the texel centre u = (i + 0.5) / 513. Besides a small mismatch
// with the builder, that leaves half a texel at each end of the map outside the knots,
// where the offset is constant: no magnification at all. The camera end of the view
// frustum is always a corner of the light-space box, so zoomed in close the ground
// nearest the camera sits in that half texel (about 13 world units): captured
// magnification 1x there against 70-90x in the neighbouring cell, i.e. shadow texels
// 80 times larger and a 4-texel filter several units wide. Shadows fade out along a
// straight line.
//
// Map u in [0, 1] onto the knots instead: texcoord = (u * 512 + 0.5) / 513. It must be
// applied to every lookup alike (casters, the shared include, Dust's receiver).
namespace RtwWarpLookup
{
inline std::string Patch(const std::string& source)
{
    if (source.find("DUST_RTW_KNOT_LOOKUP") != std::string::npos) return source;
    // tex2Dlod(warpMap, float4(<u>, 0.25f, ...   and   warpMap.SampleLevel(WarpLinear, float2(<u>, 0.25f ...
    static const std::regex pattern(
        R"(((?:tex2Dlod\s*\(\s*warpMap\s*,\s*float4|warpMap\s*\.\s*SampleLevel\s*\(\s*WarpLinear\s*,\s*float2)\s*\(\s*)([^,()]+)(\s*,\s*0\.(?:25|75)f?\b))");
    if (!std::regex_search(source, pattern)) return source;
    return "#define DUST_RTW_KNOT_LOOKUP 1\n" +
           std::regex_replace(source, pattern, "$1(($2) * (512.0 / 513.0) + (0.5 / 513.0))$3");
}

// Patches included files (rtwshadows.hlsl and shadowcaster.hlsl are pulled in by the
// skinned, terrain, foliage, construction and deferred shaders).
class PatchingInclude : public ID3DInclude
{
    ID3DInclude* inner;
    std::map<const void*, const void*> originals;   // patched buffer -> inner's buffer
public:
    explicit PatchingInclude(ID3DInclude* wrapped) : inner(wrapped) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent, LPCVOID* data, UINT* bytes) override
    {
        auto parentIt = originals.find(parent);   // nested include: hand the inner handler its own buffer
        HRESULT hr = inner->Open(type, name, parentIt == originals.end() ? parent : parentIt->second, data, bytes);
        if (FAILED(hr) || !data || !*data || !bytes) return hr;
        std::string text((const char*)*data, *bytes);
        std::string patched = Patch(text);
        if (patched == text) return hr;
        char* copy = new char[patched.size()];
        memcpy(copy, patched.data(), patched.size());
        originals[copy] = *data;
        *data = copy;
        *bytes = (UINT)patched.size();
        return hr;
    }

    HRESULT __stdcall Close(LPCVOID data) override
    {
        auto it = originals.find(data);
        if (it == originals.end()) return inner->Close(data);
        const void* original = it->second;
        originals.erase(it);
        delete[] static_cast<const char*>(data);
        return inner->Close(original);
    }
};
}
