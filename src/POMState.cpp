#include "POMState.h"
#include "DustLog.h"
#include <cstring>

namespace POMState
{

namespace {

// Layout matches the HLSL cbuffer DustPOMParams in the ShaderPatch injection.
// Two float4s for natural alignment; cfg holds the scalar knobs, samples holds
// the integer-valued knobs (encoded as floats — easier for the cbuffer).
struct POMCBData
{
    float cfg[4]     = { 1.0f, 0.02f, 0.15f, 0.85f };  // enabled, heightScale, threshold, thresholdWidth
    float samples[4] = { 8.0f, 32.0f, 0.0f, 0.0f };    // minSamples, maxSamples, pad, pad
};

ID3D11Buffer* gCB     = nullptr;
ID3D11Device* gDevice = nullptr;
POMCBData     gData;
bool          gDirty  = true;

void EnsureBuffer()
{
    if (gCB || !gDevice) return;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = sizeof(POMCBData);
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = gDevice->CreateBuffer(&desc, nullptr, &gCB);
    if (FAILED(hr))
    {
        Log("POMState: CreateBuffer failed (0x%08X)", hr);
        gCB = nullptr;
    }
}

} // anonymous namespace

void SetDevice(ID3D11Device* device)
{
    gDevice = device;
}

void Shutdown()
{
    if (gCB) { gCB->Release(); gCB = nullptr; }
    gDevice = nullptr;
}

void SetEnabled(bool enabled)        { gData.cfg[0] = enabled ? 1.0f : 0.0f; gDirty = true; }
void SetHeightScale(float scale)     { gData.cfg[1] = scale;                 gDirty = true; }
void SetThreshold(float threshold)   { gData.cfg[2] = threshold;             gDirty = true; }
void SetThresholdWidth(float width)  { gData.cfg[3] = width;                 gDirty = true; }
void SetMinSamples(int n)            { gData.samples[0] = (float)n;          gDirty = true; }
void SetMaxSamples(int n)            { gData.samples[1] = (float)n;          gDirty = true; }

void OnGBufferEnter(ID3D11DeviceContext* ctx)
{
    if (!ctx) return;
    EnsureBuffer();
    if (!gCB) return;

    if (gDirty)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(gCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, &gData, sizeof(gData));
            ctx->Unmap(gCB, 0);
            gDirty = false;
        }
    }

    ctx->PSSetConstantBuffers(8, 1, &gCB);
}

void OnGBufferLeave(ID3D11DeviceContext* ctx)
{
    if (!ctx) return;
    ID3D11Buffer* nullCB = nullptr;
    ctx->PSSetConstantBuffers(8, 1, &nullCB);
}

} // namespace POMState
