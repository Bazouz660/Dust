#include "../src/DeferredJitter.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
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

// A ground plane seen at a grazing angle. The G-buffer stores the Euclidean distance
// along the view ray, as Kenshi's does; the deferred pass rebuilds the view position
// with the game's own statement and reports how far that point is from the plane.
static const char* kScene = R"(
static const float tanX = 0.8, tanY = 0.45;
static const float3 planeNormal = float3(0, 0.98058068, 0.19611614);
static const float planeOffset = -5.0;          // dot(n, p) = planeOffset

void gbuffer_vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float3 view : TEXCOORD0)
{
	float x = (id & 1) ? 400.0 : -400.0;
	float z = (id & 2) ? -600.0 : -2.0;
	float y = (planeOffset - planeNormal.z * z) / planeNormal.y;
	view = float3(x, y, z);
	pos = float4(x / tanX, y / tanY, -z * 0.5, -z);
}
float4 gbuffer_fs(float4 pixel : SV_Position, float3 view : TEXCOORD0) : SV_Target { return length(view); }

sampler2D gBuf2 : register(s0);
void quad_vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float3 ray : TEXCOORD1, out float2 uv : TEXCOORD0)
{
	uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	ray = float3(pos.x * tanX, pos.y * tanY, -1) * 1000.0;
}
float4 main_fs(float4 pixel : SV_Position, float3 ray : TEXCOORD1, float2 texCoord : TEXCOORD0) : SV_Target
{
	float distance = tex2D(gBuf2, texCoord).r;
	if (distance <= 0) return 0;
	float4 viewPos  = float4( normalize(ray) * distance, 1.0f);
	return abs(dot(planeNormal, viewPos.xyz) - planeOffset);
}
)";

int main(int argc, char** argv)
{
    const std::string source = kScene;
    std::string patched = DeferredJitter::Patch(source);
    assert(patched != source);
    assert(DeferredJitter::Patch(patched) == patched);
    patched = DeferredJitter::Declaration() + patched;

    if (argc > 1)
    {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string game((std::istreambuf_iterator<char>(file)), {});
        std::string gamePatched = DeferredJitter::Patch(game);
        assert(gamePatched != game);
        // Only the sun pass is rewritten; the light-volume pass keeps the game's statement.
        size_t mainFs = gamePatched.find("main_fs"), lightFs = gamePatched.find("light_fs");
        size_t fixed = gamePatched.find("dustJitterRay");
        assert(mainFs != std::string::npos && lightFs != std::string::npos && fixed > mainFs && fixed < lightFs);
        assert(gamePatched.find("normalize(ray) * distance", lightFs) != std::string::npos);
        std::puts("Real game deferred.hlsl: main_fs reconstruction patched, light_fs untouched");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &ctx)));
    const UINT W = 160, H = 90;
    auto makeTarget = [&](ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv, ComPtr<ID3D11ShaderResourceView>* srv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &tex)));
        assert(SUCCEEDED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv)));
        if (srv) assert(SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &*srv)));
    };
    ComPtr<ID3D11Texture2D> gbuffer, result, staging;
    ComPtr<ID3D11RenderTargetView> gbufferRTV, resultRTV;
    ComPtr<ID3D11ShaderResourceView> gbufferSRV;
    makeTarget(gbuffer, gbufferRTV, &gbufferSRV);
    makeTarget(result, resultRTV, nullptr);
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = W; sd.Height = H; sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging)));
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;
    assert(SUCCEEDED(device->CreateSamplerState(&smp, &sampler)));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    assert(SUCCEEDED(device->CreateRasterizerState(&rd, &raster)));
    D3D11_BUFFER_DESC bd = { sizeof(DeferredJitter::FrameParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ComPtr<ID3D11Buffer> frameParams;
    assert(SUCCEEDED(device->CreateBuffer(&bd, nullptr, &frameParams)));

    auto shader = [&](const std::string& text, const char* vsEntry, const char* psEntry,
                      ComPtr<ID3D11VertexShader>& vs, ComPtr<ID3D11PixelShader>& ps) {
        auto v = Compile(text, vsEntry, "vs_4_0"); auto p = Compile(text, psEntry, "ps_4_0");
        assert(SUCCEEDED(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs)));
        assert(SUCCEEDED(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps)));
    };
    ComPtr<ID3D11VertexShader> gVS, qVS, qVSpatched;
    ComPtr<ID3D11PixelShader> gPS, qPS, qPSpatched;
    shader(source, "gbuffer_vs", "gbuffer_fs", gVS, gPS);
    shader(source, "quad_vs", "main_fs", qVS, qPS);
    shader(patched, "quad_vs", "main_fs", qVSpatched, qPSpatched);

    // Worst distance of the rebuilt points from the plane, for a given G-buffer jitter.
    auto worstError = [&](float jx, float jy, bool usePatched) {
        const float clear[4] = { 0, 0, 0, 0 };
        ctx->ClearState();
        ctx->ClearRenderTargetView(gbufferRTV.Get(), clear);
        ctx->OMSetRenderTargets(1, gbufferRTV.GetAddressOf(), nullptr);
        D3D11_VIEWPORT jittered = { jx, jy, float(W), float(H), 0, 1 };   // what Dust does to the scene pass
        ctx->RSSetViewports(1, &jittered);
        ctx->RSSetState(raster.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(gVS.Get(), nullptr, 0);
        ctx->PSSetShader(gPS.Get(), nullptr, 0);
        ctx->Draw(4, 0);

        ctx->ClearState();
        ctx->OMSetRenderTargets(1, resultRTV.GetAddressOf(), nullptr);
        D3D11_VIEWPORT full = { 0, 0, float(W), float(H), 0, 1 };           // the lighting quad is never jittered
        ctx->RSSetViewports(1, &full);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(usePatched ? qVSpatched.Get() : qVS.Get(), nullptr, 0);
        ctx->PSSetShader(usePatched ? qPSpatched.Get() : qPS.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, gbufferSRV.GetAddressOf());
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        const DeferredJitter::FrameParams params = { jx, jy, 0, 1 };
        ctx->UpdateSubresource(frameParams.Get(), 0, nullptr, &params, 0, 0);
        ctx->PSSetConstantBuffers(DeferredJitter::Slot, 1, frameParams.GetAddressOf());
        ctx->Draw(3, 0);
        ctx->CopyResource(staging.Get(), result.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        float worst = 0;
        // Interior only: the outermost pixels can be uncovered by the shifted geometry.
        for (UINT y = 2; y + 2 < H; ++y)
            for (UINT x = 2; x + 2 < W; ++x)
                worst = (std::max)(worst, ((const float*)((const char*)map.pData + y * map.RowPitch))[x]);
        ctx->Unmap(staging.Get(), 0);
        return worst;
    };

    const float still = worstError(0, 0, false);
    assert(still < 1e-2f);                                     // no jitter: the game's reconstruction is exact
    for (auto j : { std::pair<float, float>{0.37f, -0.41f}, {-0.5f, 0.5f}, {0.25f, 0.0f}, {0.0f, -0.45f} })
    {
        float vanilla = worstError(j.first, j.second, false);
        float fixed = worstError(j.first, j.second, true);
        std::printf("jitter (%+.2f, %+.2f): worst distance from the surface %.3f units vanilla, %.4f corrected\n",
                    j.first, j.second, vanilla, fixed);
        assert(fixed < 1e-2f);
        if (j.second != 0) assert(vanilla > 0.5f);             // grazing ground: whole units off the surface
    }
    assert(worstError(0, 0, true) < 1e-2f);
    std::puts("Deferred jitter: world positions are rebuilt on the jittered sample's own ray");
}
