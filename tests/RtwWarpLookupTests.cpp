#include "../src/RtwWarpLookup.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

static ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(hr));
    return code;
}

static size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// Both lookup spellings the game uses, without its source.
static const char* kLookups = R"(
cbuffer P : register(b0) { float probe; };
sampler2D warpMap : register(s0);
float2 Legacy(float2 ts) {
	ts.x += tex2Dlod(warpMap, float4(ts.x, 0.25f, 0, 0)).x;
	ts.y -= tex2Dlod(warpMap, float4(1-ts.y, 0.75f, 0, 0)).x;
	return ts;
}
void quad_vs(uint id : SV_VertexID, out float4 pos : SV_Position)
{
	float2 uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 probe_fs(float4 pixel : SV_Position) : SV_Target { return Legacy(float2(probe, 0)).x - probe; }
)";
static const char* kModern =
    "\tts.x += warpMap.SampleLevel(WarpLinear, float2(ts.x, 0.25f), 0).x;\n"
    "\tts.y -= warpMap.SampleLevel(WarpLinear, float2(1-ts.y, 0.75f), 0).x;\n";

struct FakeInclude : ID3DInclude
{
    std::string text;
    const void* closed = nullptr;
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR, LPCVOID, LPCVOID* data, UINT* bytes) override
    { *data = text.data(); *bytes = (UINT)text.size(); return S_OK; }
    HRESULT __stdcall Close(LPCVOID data) override { closed = data; return S_OK; }
};

int main(int argc, char** argv)
{
    const std::string source = kLookups;
    const std::string patched = RtwWarpLookup::Patch(source);
    assert(Count(patched, "(512.0 / 513.0) + (0.5 / 513.0)") == 2);
    assert(patched.find("((1-ts.y) * (512.0 / 513.0)") != std::string::npos);
    assert(RtwWarpLookup::Patch(patched) == patched);
    assert(Count(RtwWarpLookup::Patch(kModern), "(512.0 / 513.0) + (0.5 / 513.0)") == 2);
    assert(RtwWarpLookup::Patch("float4 main() : SV_Target { return 0; }") == "float4 main() : SV_Target { return 0; }");

    // Included files are patched too, and the inner handler gets its own buffer back.
    FakeInclude inner;
    inner.text = kModern;
    RtwWarpLookup::PatchingInclude wrapper(&inner);
    LPCVOID data = nullptr; UINT bytes = 0;
    assert(SUCCEEDED(wrapper.Open(D3D_INCLUDE_LOCAL, "rtwshadows.hlsl", nullptr, &data, &bytes)));
    assert(data != inner.text.data());
    assert(std::string((const char*)data, bytes) == RtwWarpLookup::Patch(kModern));
    wrapper.Close(data);
    assert(inner.closed == inner.text.data());
    inner.text = "float unrelated;"; inner.closed = nullptr;
    assert(SUCCEEDED(wrapper.Open(D3D_INCLUDE_LOCAL, "gbuffer.hlsl", nullptr, &data, &bytes)));
    assert(data == inner.text.data());
    wrapper.Close(data);
    assert(inner.closed == inner.text.data());

    if (argc > 1)
    {
        const std::filesystem::path materials = argv[1];
        const std::pair<const char*, size_t> files[] = {
            {"common/rtwshadows.hlsl", 4}, {"common/shadowcaster.hlsl", 2}, {"common/rtwtessellator.hlsl", 2} };
        for (const auto& [name, lookups] : files)
        {
            std::ifstream file(materials / name);
            assert(file.good());
            std::string game((std::istreambuf_iterator<char>(file)), {});
            size_t found = Count(RtwWarpLookup::Patch(game), "(512.0 / 513.0) + (0.5 / 513.0)");
            if (found != lookups) std::fprintf(stderr, "%s: %zu lookups patched, expected %zu\n", name, found, lookups);
            assert(found == lookups);
        }
        // Nothing else in the game samples the warp map.
        size_t others = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(materials))
        {
            if (entry.path().extension() != ".hlsl") continue;
            std::ifstream file(entry.path());
            std::string text((std::istreambuf_iterator<char>(file)), {});
            std::string name = entry.path().filename().string();
            bool known = name == "rtwshadows.hlsl" || name == "shadowcaster.hlsl" || name == "rtwtessellator.hlsl";
            if (!known && RtwWarpLookup::Patch(text) != text) ++others;
            if (!known && (text.find("tex2Dlod(warpMap") != std::string::npos || text.find("warpMap.Sample") != std::string::npos))
            { std::fprintf(stderr, "unexpected warp lookup in %s\n", name.c_str()); assert(false); }
        }
        assert(others == 0);
        std::puts("Installed game: all 8 warp lookups in 3 files patched; no other shader samples the warp map");
    }

    // A 513-knot map sampled with a linear clamp sampler, as the game does.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &ctx)));
    std::vector<float> knots(513 * 2);
    for (int i = 0; i < 513 * 2; ++i) knots[i] = float(i % 513);          // offset == knot index
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 513; td.Height = 2; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { knots.data(), 513 * sizeof(float), 0 };
    ComPtr<ID3D11Texture2D> warp;
    assert(SUCCEEDED(device->CreateTexture2D(&td, &init, &warp)));
    ComPtr<ID3D11ShaderResourceView> warpView;
    assert(SUCCEEDED(device->CreateShaderResourceView(warp.Get(), nullptr, &warpView)));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    assert(SUCCEEDED(device->CreateSamplerState(&sd, &sampler)));
    td.Width = td.Height = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)));
    ComPtr<ID3D11RenderTargetView> rtv;
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging)));
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ComPtr<ID3D11Buffer> constants;
    assert(SUCCEEDED(device->CreateBuffer(&bd, nullptr, &constants)));
    auto vsCode = Compile(source, "quad_vs", "vs_4_0");
    ComPtr<ID3D11VertexShader> vs;
    assert(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));

    auto knotAt = [&](const std::string& shader, float u) {
        auto psCode = Compile(shader, "probe_fs", "ps_4_0");
        ComPtr<ID3D11PixelShader> ps;
        assert(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)));
        const float params[4] = { u, 0, 0, 0 };
        ctx->ClearState();
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, params, 0, 0);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport = { 0, 0, 1, 1, 0, 1 };
        ctx->RSSetViewports(1, &viewport);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, warpView.GetAddressOf());
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Draw(3, 0);
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        float value = *static_cast<const float*>(map.pData);
        ctx->Unmap(staging.Get(), 0);
        return value;
    };

    // Patched: u = i / 512 reads knot i exactly, and the map's last stretch still interpolates.
    for (int i : {0, 1, 256, 511, 512}) assert(std::fabs(knotAt(patched, i / 512.f) - i) < 1e-2f);
    assert(std::fabs(knotAt(patched, 511.5f / 512.f) - 511.5f) < 1e-2f);
    // Vanilla: the last half texel is flat (no magnification), and knots sit off the builder's grid.
    assert(std::fabs(knotAt(source, 511.75f / 512.f) - knotAt(source, 1.0f)) < 1e-3f);
    assert(std::fabs(knotAt(source, 128 / 512.f) - 128) > 0.2f);
    std::puts("RTW warp lookup: knot i is read at u = i / 512 and the ends of the map keep interpolating");
}
