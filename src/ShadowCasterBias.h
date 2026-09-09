#pragma once

#include <d3d11.h>
#include <cmath>
#include <string>

namespace ShadowCasterBias
{
// PS b13 is reserved for the caster pass (VS b13 belongs to motion vectors).
constexpr UINT Slot = 13;

inline std::string Patch(const std::string& source)
{
    const char* anchor = "slopeBias * length(g)";
    size_t pos = source.find(anchor);
    if (pos == std::string::npos || source.find("dustCasterScale") != std::string::npos)
        return source;
    std::string result = source;
    // Scaling BEFORE the existing clamp preserves the game's maximum bias.
    result.replace(pos, std::char_traits<char>::length(anchor),
        "slopeBias * length(g)\n#ifdef RTW\n * max(dustCasterScale, 1.0)\n#endif\n");
    // Test RTW at the expression, after any defines in the source itself.
    // The unused cbuffer disappears from non-RTW compiled variants.
    return "cbuffer DustCasterParams : register(b13) { float dustCasterScale; };\n" + result;
}

class Binding
{
    ID3D11Buffer* buffer = nullptr;
    float cachedScale = 0;
    bool bound = false;
public:
    Binding() = default;
    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;
    ~Binding() { Reset(); }

    void Reset()
    {
        if (buffer) buffer->Release();
        buffer = nullptr;
        cachedScale = 0;
        bound = false;
    }

    // Called on every OM bind, including the no-override fast path. ClearState
    // may already have removed our buffer; don't clobber another owner's b13.
    void EndPass(ID3D11DeviceContext* context)
    {
        if (!bound) return;
        ID3D11Buffer* current = nullptr;
        context->PSGetConstantBuffers(Slot, 1, &current);
        if (current == buffer)
        {
            ID3D11Buffer* empty = nullptr;
            context->PSSetConstantBuffers(Slot, 1, &empty);
        }
        if (current) current->Release();
        bound = false;
    }

    // OGRE reflects injected cbuffers and can bind its own zero-filled b13
    // during material setup. Pin ours after that setup, immediately before
    // each caster draw. The immutable contents never need a per-draw upload.
    void BeforeDraw(ID3D11DeviceContext* context)
    {
        if (bound) context->PSSetConstantBuffers(Slot, 1, &buffer);
    }

    bool BeginPass(ID3D11DeviceContext* context, float actualAtlasScale)
    {
        // Never reduce vanilla bias when downsampling. A missing/unbound CB
        // reads as zero, which the shader treats as the vanilla factor of one.
        if (!(actualAtlasScale > 1.0f) || !std::isfinite(actualAtlasScale)) return true;
        if (!buffer || cachedScale != actualAtlasScale)
        {
            const float data[4] = { actualAtlasScale, 0, 0, 0 };
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = sizeof(data);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial = { data, 0, 0 };
            ID3D11Device* device = nullptr;
            context->GetDevice(&device);
            ID3D11Buffer* replacement = nullptr;
            HRESULT hr = device->CreateBuffer(&desc, &initial, &replacement);
            device->Release();
            if (FAILED(hr)) return false;
            if (buffer) buffer->Release();
            buffer = replacement;
            cachedScale = actualAtlasScale;
        }
        context->PSSetConstantBuffers(Slot, 1, &buffer);
        bound = true;
        return true;
    }
};
}
