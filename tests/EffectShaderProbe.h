#pragma once
#include "../src/DustAPI.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;

// Real effect DLL, real shaders and float targets on WARP. No game hooks.
struct EffectShaderProbe {
    inline static EffectShaderProbe* active = nullptr;
    static constexpr UINT W = 24, H = 16;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> scene, depth, output, staging;
    ComPtr<ID3D11ShaderResourceView> sceneSRV, depthSRV;
    ComPtr<ID3D11RenderTargetView> outputRTV;
    HMODULE module = nullptr;
    DustEffectDesc effect = {};
    DustHostAPI host = {};
    std::filesystem::path shaderDir;
    bool hasDepth = true;
    std::string lastTarget;

    static ID3DBlob* Compile(const char* path, const char* entry, const char* target) {
        auto file = active->shaderDir / std::filesystem::path(path).filename();
        if (!std::filesystem::exists(file) && file.filename() == "fullscreen_vs.hlsl")
            file = "../effects/ssao/shaders/fullscreen_vs.hlsl";
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompileFromFile(file.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
        assert(SUCCEEDED(hr)); return code.Detach();
    }
    EffectShaderProbe(const char* folder, const char* dll) {
        active = this;
        assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)));
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &scene)));
        assert(SUCCEEDED(device->CreateShaderResourceView(scene.Get(), nullptr, &sceneSRV)));
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &output)));
        assert(SUCCEEDED(device->CreateRenderTargetView(output.Get(), nullptr, &outputRTV)));
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging)));
        td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &depth)));
        assert(SUCCEEDED(device->CreateShaderResourceView(depth.Get(), nullptr, &depthSRV)));
        host.apiVersion = DUST_API_VERSION;
        host.Log = [](const char*, ...) {};
        host.CompileShaderFromFile = Compile;
        host.CreateConstantBuffer = [](ID3D11Device* dev, uint32_t bytes) {
            D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = (bytes + 15) & ~15u;
            bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ID3D11Buffer* cb = nullptr; assert(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb))); return cb;
        };
        host.UpdateConstantBuffer = [](ID3D11DeviceContext* c, ID3D11Buffer* cb, const void* data, uint32_t bytes) {
            D3D11_MAPPED_SUBRESOURCE map = {};
            assert(SUCCEEDED(c->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)));
            std::memcpy(map.pData, data, bytes); c->Unmap(cb, 0);
        };
        host.GetSRV = [](const char*) { return active->hasDepth ? active->depthSRV.Get() : nullptr; };
        host.GetRTV = [](const char* name) { active->lastTarget = name; return active->outputRTV.Get(); };
        host.GetSceneCopy = [](ID3D11DeviceContext*, const char*) { return active->sceneSRV.Get(); };
        host.SaveState = [](ID3D11DeviceContext*) {};
        host.RestoreState = host.SaveState;
        shaderDir = std::filesystem::path("../effects") / folder / "shaders";
        auto path = std::filesystem::absolute(std::filesystem::path("../effects") / folder / "build/Release" / dll);
        module = LoadLibraryW(path.c_str()); assert(module);
        auto create = (PFN_DustEffectCreate)GetProcAddress(module, "DustEffectCreate"); assert(create);
        assert(create(&effect) == 0);
        assert(effect.Init(device.Get(), W, H, &host) == 0);
    }
    ~EffectShaderProbe() { ctx->ClearState(); effect.Shutdown(); FreeLibrary(module); active = nullptr; }
    template<class T> T& Setting(const char* key) {
        for (uint32_t i = 0; i < effect.settingCount; ++i)
            if (effect.settings[i].iniKey && !std::strcmp(effect.settings[i].iniKey, key))
                return *static_cast<T*>(effect.settings[i].valuePtr);
        assert(false); std::abort();
    }
    std::vector<Pixel> Render(const std::vector<Pixel>& input, float distance,
                             DustInjectionPoint point = DUST_INJECT_POST_LIGHTING) {
        return RenderDepths(input, std::vector<float>(W * H, distance), point);
    }
    std::vector<Pixel> RenderDepths(const std::vector<Pixel>& input, const std::vector<float>& depths,
                                  DustInjectionPoint point = DUST_INJECT_POST_LIGHTING) {
        assert(depths.size() == W * H);
        ctx->ClearState();
        ctx->UpdateSubresource(scene.Get(), 0, nullptr, input.data(), W * sizeof(Pixel), 0);
        ctx->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(), W * sizeof(float), 0);
        // The game target already contains the input when an effect skips work.
        ctx->CopyResource(output.Get(), scene.Get());
        DustFrameContext frame = {}; frame.device = device.Get(); frame.context = ctx.Get();
        frame.width = W; frame.height = H; frame.point = point; frame.timing = DUST_TIMING_POST;
        effect.postExecute(&frame, &host);
        ctx->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        std::vector<Pixel> result(W * H);
        for (UINT y = 0; y < H; ++y)
            std::memcpy(result.data() + y * W, (char*)map.pData + y * map.RowPitch, W * sizeof(Pixel));
        ctx->Unmap(staging.Get(), 0); return result;
    }
};

inline void AssertImagesNear(const std::vector<Pixel>& a, const std::vector<Pixel>& b, float epsilon = 2e-5f) {
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(a[i][c]) || std::abs(a[i][c] - b[i][c]) > epsilon) {
                std::fprintf(stderr, "Pixel %zu channel %d: actual %g expected %g\n", i, c, a[i][c], b[i][c]);
                assert(false);
            }
        }
}
