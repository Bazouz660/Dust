#include "../src/DustAPI.h"
#include <d3dcompiler.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

static std::filesystem::path gShaderDir;
static ID3D11ShaderResourceView* gScene = nullptr;
static ID3D11ShaderResourceView* gDepth = nullptr;
static ID3D11RenderTargetView* gHDR = nullptr;
static ID3D11RenderTargetView* gLDR = nullptr;
static ID3D11VertexShader* gVS = nullptr;
static bool gTracking = false;
static int gAttempts = 0, gFailAfter = -1;
using CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTextureFn gCreateTexture = nullptr;

static HRESULT STDMETHODCALLTYPE TrackTexture(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out)
{
    if (gTracking && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS)))
    {
        ++gAttempts;
        if (gFailAfter == 0) { *out = nullptr; return E_OUTOFMEMORY; }
        if (gFailAfter > 0) --gFailAfter;
    }
    return gCreateTexture(dev, desc, data, out);
}

static ID3DBlob* CompileFile(const char* path, const char* entry, const char* target)
{
    auto file = gShaderDir / std::filesystem::path(path).filename();
    if (!std::filesystem::exists(file) && file.filename() == "fullscreen_vs.hlsl")
        file = "../effects/ssao/shaders/fullscreen_vs.hlsl";
    ID3DBlob* blob = nullptr; ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompileFromFile(file.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                         entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    if (errors) errors->Release();
    return SUCCEEDED(hr) ? blob : nullptr;
}
static ID3D11Buffer* CreateCB(ID3D11Device* dev, uint32_t bytes)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = (bytes + 15) & ~15u; desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr; assert(SUCCEEDED(dev->CreateBuffer(&desc, nullptr, &cb))); return cb;
}
static void UpdateCB(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, const void* data, uint32_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    assert(SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)));
    std::memcpy(mapped.pData, data, bytes); ctx->Unmap(cb, 0);
}
static void IgnoreLog(const char*, ...) {}
static void Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gVS, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0); ctx->Draw(3, 0);
}
static void Fixtures(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT width, UINT height)
{
    gTracking = false;
    ctx->ClearState();
    if (gScene) gScene->Release(); if (gDepth) gDepth->Release();
    if (gHDR) gHDR->Release(); if (gLDR) gLDR->Release();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const DXGI_FORMAT formats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
    for (int i = 0; i < 4; ++i)
    {
        desc.Format = formats[i];
        ID3D11Texture2D* tex = nullptr;
        assert(SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &tex)));
        ID3D11RenderTargetView* rtv = nullptr;
        assert(SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv)));
        const float color[4] = {.05f,.05f,1.f,1.f}; ctx->ClearRenderTargetView(rtv, color);
        if (i < 2)
        {
            assert(SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, i == 0 ? &gScene : &gDepth)));
            rtv->Release();
        }
        else if (i == 2) gHDR = rtv;
        else gLDR = rtv;
        tex->Release();
    }
    gTracking = true;
}

int main()
{
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &dev, nullptr, &ctx)));
    gShaderDir = "../effects/ssao/shaders";
    ID3DBlob* vs = CompileFile("fullscreen_vs.hlsl", "main", "vs_5_0"); assert(vs);
    assert(SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gVS)));
    vs->Release();
    // Observe only this standalone test device, never the game's device/vtables.
    void** vtable = *reinterpret_cast<void***>(dev);
    DWORD oldProtect = 0;
    assert(VirtualProtect(vtable+5, sizeof(void*), PAGE_READWRITE, &oldProtect));
    gCreateTexture = reinterpret_cast<CreateTextureFn>(vtable[5]);
    vtable[5] = reinterpret_cast<void*>(&TrackTexture);
    DWORD ignored = 0; assert(VirtualProtect(vtable+5, sizeof(void*), oldProtect, &ignored));

    DustHostAPI host = {};
    host.apiVersion = DUST_API_VERSION; host.Log = IgnoreLog;
    host.CompileShaderFromFile = CompileFile; host.CreateConstantBuffer = CreateCB; host.UpdateConstantBuffer = UpdateCB;
    host.GetSRV = [](const char* name) { return std::strcmp(name, DUST_RESOURCE_DEPTH) == 0 ? gDepth : gScene; };
    host.GetRTV = [](const char* name) { return std::strcmp(name, DUST_RESOURCE_LDR_RT) == 0 ? gLDR : gHDR; };
    host.GetSceneCopy = [](ID3D11DeviceContext*, const char*) { return gScene; };
    host.SaveState = [](ID3D11DeviceContext*) {}; host.RestoreState = host.SaveState;
    host.DrawFullscreenTriangle = Draw;
    host.BindSRV = [](ID3D11DeviceContext*, uint32_t, ID3D11ShaderResourceView*, ID3D11SamplerState*) {};
    host.UnbindSRV = [](ID3D11DeviceContext*, uint32_t) {};
    const char* folders[] = {"ssao","ssil","rtgi","dof","bloom","smaa","clarity"};
    const char* dlls[] = {"DustSSAO","DustSSIL","DustRTGI","DustDOF","DustBloom","DustSMAA","DustClarity"};
    for (int i = 0; i < 7; ++i)
    {
        std::printf("%s: ", dlls[i]); std::fflush(stdout);
        gShaderDir = std::filesystem::path("../effects") / folders[i] / "shaders";
        const auto dll = std::filesystem::absolute(std::filesystem::path("../effects") / folders[i] / "build/Release" / (std::string(dlls[i])+".dll"));
        HMODULE module = LoadLibraryW(dll.c_str()); assert(module);
        auto create = reinterpret_cast<int(*)(DustEffectDesc*)>(GetProcAddress(module, "DustEffectCreate")); assert(create);
        DustEffectDesc effect = {}; assert(create(&effect) == 0);
        bool* enabled = nullptr;
        for (uint32_t j = 0; j < effect.settingCount; ++j)
            if (effect.settings[j].type == DUST_SETTING_BOOL && std::strcmp(effect.settings[j].name, "Enabled") == 0)
                enabled = static_cast<bool*>(effect.settings[j].valuePtr);
        assert(enabled); *enabled = false;
        Fixtures(dev, ctx, 64, 64);
        gAttempts = 0;
        assert(effect.Init(dev, 64, 64, &host) == 0);
        assert(gAttempts == 0); // only small immutable/fallback textures at Init
        effect.OnResolutionChanged(dev, 80, 64);
        assert(gAttempts == 0); // disabled resize must not allocate
        Fixtures(dev, ctx, 80, 64);
        DustFrameContext frame = {}; frame.device = dev; frame.context = ctx;
        frame.width = 80; frame.height = 64; frame.camera.valid = 1; frame.camera.tanHalfFov = .7f;
        frame.camera.nearZ = .1f; frame.camera.farZ = 2500;
        for (int k = 0; k < 16; ++k) frame.camera.inverseView[k] = k%5 == 0 ? 1.f : 0.f;
        auto render = [&] {
            ctx->ClearState();
            if (effect.preExecute) effect.preExecute(&frame, &host);
            if (effect.postExecute) effect.postExecute(&frame, &host);
            ++frame.frameIndex;
        };
        render(); assert(gAttempts == 0);
        *enabled = true;
        gFailAfter = 1; render(); // fail after one successful allocation
        assert(gAttempts == 2);
        const int failedAttempts = gAttempts;
        render(); assert(gAttempts == failedAttempts); // backoff prevents repeated allocations
        gFailAfter = -1;
        Sleep(1100);
        render(); assert(gAttempts > failedAttempts); // successful retry without reinitializing plugin
        const int readyAttempts = gAttempts;
        render(); assert(gAttempts == readyAttempts); // reuse the complete set
        *enabled = false; render(); assert(gAttempts == readyAttempts);
        effect.OnResolutionChanged(dev, 96, 64); assert(gAttempts == readyAttempts);
        Fixtures(dev, ctx, 96, 64); frame.width = 96;
        render(); assert(gAttempts == readyAttempts);
        *enabled = true; render(); assert(gAttempts > readyAttempts);
        // Cold debug paths must prepare their own targets too (notably DOF
        // and Clarity, whose debug callbacks replace the ordinary render).
        for (uint32_t j = 0; j < effect.settingCount; ++j)
        {
            auto& setting = effect.settings[j];
            if (!setting.iniKey || std::strcmp(setting.iniKey, "DebugView") != 0) continue;
            if (setting.type == DUST_SETTING_BOOL) *static_cast<bool*>(setting.valuePtr) = true;
            else if (setting.type == DUST_SETTING_INT || setting.type == DUST_SETTING_ENUM)
                *static_cast<int*>(setting.valuePtr) = 1;
            else continue;
            const int beforeDebug = gAttempts;
            effect.OnResolutionChanged(dev, 112, 64);
            assert(gAttempts == beforeDebug);
            Fixtures(dev, ctx, 112, 64); frame.width = 112;
            render(); assert(gAttempts > beforeDebug);
            break;
        }
        assert(SUCCEEDED(dev->GetDeviceRemovedReason()));
        ctx->ClearState(); effect.Shutdown(); FreeLibrary(module);
        std::puts("cold init, resize, allocation failure, retry, enable: PASS");
    }
    gTracking = false;
    assert(VirtualProtect(vtable+5, sizeof(void*), PAGE_READWRITE, &oldProtect));
    vtable[5] = reinterpret_cast<void*>(gCreateTexture);
    assert(VirtualProtect(vtable+5, sizeof(void*), oldProtect, &ignored));
    ctx->ClearState(); ctx->Flush();
    gScene->Release(); gDepth->Release(); gHDR->Release(); gLDR->Release(); gVS->Release();
    ctx->Release(); dev->Release();
}
