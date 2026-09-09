#pragma once
#include <d3d11.h>
#include <cstdint>

// GPU-only snapshots of the deferred lighting CB. Command ordering keeps each
// pose paired with its image even when the GPU is several frames behind.
class RTGICamera
{
    ID3D11Buffer* buffers[2] = {};
    UINT bytes = 0;
    int write = 0;
    uint64_t captured = 0, committed = 0;
    bool currentValid = false, previousValid = false;
public:
    void Reset() {
        for (auto& b : buffers) { if (b) b->Release(); b = nullptr; }
        bytes = 0; write = 0; currentValid = previousValid = false;
    }
    void Capture(ID3D11DeviceContext* ctx, uint64_t frame) {
        currentValid = false;
        ID3D11Buffer* source = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &source);
        if (!source) return;
        D3D11_BUFFER_DESC desc = {}; source->GetDesc(&desc);
        if (desc.ByteWidth < 192) { source->Release(); return; }
        if (bytes != desc.ByteWidth) Reset();
        if (!buffers[0]) {
            ID3D11Device* device = nullptr; ctx->GetDevice(&device);
            D3D11_BUFFER_DESC copy = {};
            copy.ByteWidth = desc.ByteWidth; copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const bool ok = SUCCEEDED(device->CreateBuffer(&copy, nullptr, &buffers[0]))
                && SUCCEEDED(device->CreateBuffer(&copy, nullptr, &buffers[1]));
            device->Release();
            if (!ok) { Reset(); source->Release(); return; }
            bytes = desc.ByteWidth;
        }
        ctx->CopyResource(buffers[write], source); source->Release();
        captured = frame; currentValid = true;
    }
    bool CurrentReady(uint64_t frame) const { return currentValid && captured == frame; }
    bool HistoryReady(uint64_t frame) const { return CurrentReady(frame) && previousValid && committed + 1 == frame; }
    ID3D11Buffer* Current() const { return buffers[write]; }
    ID3D11Buffer* Previous() const { return buffers[1-write]; }
    void Commit(uint64_t frame) {
        previousValid = CurrentReady(frame); committed = frame;
        if (previousValid) write = 1-write;
        currentValid = false;
    }
};
