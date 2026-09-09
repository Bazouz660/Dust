#include "../src/ShadowCasterBias.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;

static ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry,
                               const char* target, const D3D_SHADER_MACRO* defines = nullptr)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, defines, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(hr));
    return code;
}

int main(int argc, char** argv)
{
    const D3D_SHADER_MACRO rtw[] = { {"RTW", "1"}, {nullptr, nullptr} };
    // Optional local game source: compile real opaque/alpha/construction
    // variants in both modes without copying proprietary shaders into git.
    if (argc > 1)
    {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string source((std::istreambuf_iterator<char>(file)), {});
        auto patched = ShadowCasterBias::Patch(source);
        assert(patched != source);
        for (int bits = 0; bits < 8; ++bits)
        {
            D3D_SHADER_MACRO defs[5] = {};
            int count = 0;
            if (bits & 1) defs[count++] = {"RTW", "1"};
            if (bits & 2) defs[count++] = {"ALPHA", "1"};
            if (bits & 4) defs[count++] = {"CONSTRUCTION", "1"};
            if (bits & 6) defs[count++] = {"TEXCOORDS", "1"};
            Compile(patched, "shadow_fs", "ps_4_0", defs);
        }
        std::puts("Real game shadowcaster.hlsl: 8 variants compiled");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)));
    auto vsCode = Compile(R"(
cbuffer TestParams : register(b0) { float slope; };
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float depth : TEXCOORD0) {
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    depth = uv.x * slope;
})", "main", "vs_4_0");
    ComPtr<ID3D11VertexShader> vs;
    assert(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
    const std::string source = R"(
float shadow_fs(float4 fragment : SV_Position, float depth : TEXCOORD0) : SV_Target {
    float originalDepth = depth;
    float fixedBias = 0.00003;
    float slopeBias = 6.0;
    float maxSlopeBias = 0.002;
    float2 g = float2(ddx(depth), ddy(depth));
    depth += min(maxSlopeBias, slopeBias * length(g));
    depth += fixedBias;
    return depth - originalDepth;
})";
    auto patched = ShadowCasterBias::Patch(source);
    assert(ShadowCasterBias::Patch(patched) == patched);
    assert(ShadowCasterBias::Patch("unrelated shader") == "unrelated shader");
    ComPtr<ID3D11PixelShader> ps[3];
    for (int i = 0; i < 3; ++i)
    {
        auto code = Compile(i == 0 ? source : patched, "shadow_fs", "ps_4_0", i == 2 ? rtw : nullptr);
        assert(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps[i])));
    }
    // A source-local define must also work, not only D3DCompile's macro array.
    Compile(ShadowCasterBias::Patch("#define RTW\n" + source), "shadow_fs", "ps_4_0");

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 4; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)));
    ComPtr<ID3D11RenderTargetView> rtv;
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging)));
    ShadowCasterBias::Binding binding;
    for (float slope : {0.1f, 4.0f})
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        float values[4] = {slope, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {values, 0, 0};
        ComPtr<ID3D11Buffer> params;
        assert(SUCCEEDED(device->CreateBuffer(&bd, &init, &params)));
        ctx->VSSetConstantBuffers(0, 1, params.GetAddressOf());
        for (UINT size : {1024u, 2048u, 4096u, 6144u, 8192u, 12288u, 16384u, 4096u})
        {
            D3D11_VIEWPORT vp = {0, 0, (float)size, (float)size, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            for (int variant = 0; variant < 4; ++variant)
            {
                binding.EndPass(ctx.Get());
                assert(binding.BeginPass(ctx.Get(), variant == 3 ? 1.0f : size / 4096.0f));
                // Simulate OGRE's material setup overwriting the injected CB.
                ID3D11Buffer* empty = nullptr;
                ctx->PSSetConstantBuffers(13, 1, &empty);
                binding.BeforeDraw(ctx.Get());
                ctx->VSSetShader(vs.Get(), nullptr, 0);
                ctx->PSSetShader(ps[variant == 3 ? 2 : variant].Get(), nullptr, 0);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->Draw(3, 0);
                ctx->CopyResource(staging.Get(), target.Get());
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
                float actual = *(const float*)mapped.pData;
                ctx->Unmap(staging.Get(), 0);
                UINT biasResolution = variant == 2 && size > 4096 ? 4096 : size;
                float expected = 0.00003f + (std::min)(0.002f, 6.0f * slope / biasResolution);
                assert(std::abs(actual - expected) < 1e-7f);
            }
        }
        // ClearState, foreign slot ownership and disabling the override.
        ctx->ClearState();
        ctx->PSSetConstantBuffers(13, 1, params.GetAddressOf());
        binding.EndPass(ctx.Get());
        ComPtr<ID3D11Buffer> current;
        ctx->PSGetConstantBuffers(13, 1, &current);
        assert(current.Get() == params.Get());
        assert(binding.BeginPass(ctx.Get(), 4));
        binding.EndPass(ctx.Get());
        current.Reset(); ctx->PSGetConstantBuffers(13, 1, &current);
        assert(!current);
        assert(binding.BeginPass(ctx.Get(), 1));
        ctx->PSGetConstantBuffers(13, 1, &current);
        assert(!current);
    }
    ctx->ClearState();
    binding.Reset();
    std::puts("WARP: RTW bias stable across 1024-16384; native/CSM behavior and cap preserved");
}
