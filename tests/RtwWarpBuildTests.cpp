#include "../src/RtwWarpBuild.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
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

// The builder's logic for one axis, without the game's source: N importance texels,
// N + 1 knots, each knot's clip-space location written as an offset from its own.
static const char* kBuilder = R"(
cbuffer P : register(b0) { int map_size; };
Texture2D<float> importance : register(t0);
void quad_vs(uint id : SV_VertexID, out float4 pos : SV_Position)
{
	float2 uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
int2 Region(int size) {
	int first_u = size, last_u = 0;
	for (int u = 0; u < size; u++)
		if (importance.Load(int3(u, 0, 0)) > 0) { first_u = min(first_u, u); last_u = max(last_u, u); }
	return int2(first_u, last_u + 1);
}
float Weight(int u0, int u1) {
	float ret = 0;
	for (int u = u0; u < u1; u++) ret += max(importance.Load(int3(u, 0, 0)), 0.001f);
	return ret;
}
float4 rtw_build(float4 position : SV_Position) : SV_Target
{
	int my_u = (int)position.x;
	int2 range = Region(map_size);
	float st_loc = lerp(-1.0f, 1.0f, floor(position.x) / map_size);
	float my_loc = st_loc;
	if(my_u < range.x){
		my_loc = -1.05f;
	}
	else if(my_u >= range.y){
		my_loc = 1.05f;
	}
	else {
		float w0 = Weight(range.x, my_u);
		float w1 = Weight(my_u, range.y);
		my_loc = lerp(-1.0f, 1.0f, w0 / (w0 + w1));
	}
	return float4(my_loc, 0, 0, 1);
}
)";

int main(int argc, char** argv)
{
    const std::string source = kBuilder;
    const std::string patched = RtwWarpBuild::Patch(source);
    assert(patched != source);
    assert(RtwWarpBuild::Patch(patched) == patched);
    assert(RtwWarpBuild::Patch("float4 main() : SV_Target { return 0; }") == "float4 main() : SV_Target { return 0; }");

    if (argc > 1)
    {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string game((std::istreambuf_iterator<char>(file)), {});
        std::string gamePatched = RtwWarpBuild::Patch(game);
        assert(gamePatched != game);
        // The game's own compile flags are not known here: whichever of these builds the
        // original must also build the patched source.
        int built = 0;
        for (UINT flags : {(UINT)D3DCOMPILE_OPTIMIZATION_LEVEL1, (UINT)D3DCOMPILE_SKIP_OPTIMIZATION,
                           (UINT)D3DCOMPILE_PREFER_FLOW_CONTROL, (UINT)D3DCOMPILE_OPTIMIZATION_LEVEL3})
        {
            auto build = [&](const std::string& text) {
                ComPtr<ID3DBlob> code, errors;
                return SUCCEEDED(D3DCompile(text.data(), text.size(), nullptr, nullptr, nullptr, "rtw_build", "ps_4_0",
                    D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | flags, 0, &code, &errors));
            };
            if (!build(game)) continue;
            assert(build(gamePatched));
            ++built;
        }
        std::printf("Real game rtwshadows.hlsl: rtw_build patched; %d flag sets compile it like the original\n", built);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &ctx)));
    const int N = 8;
    auto vsCode = Compile(source, "quad_vs", "vs_4_0");
    ComPtr<ID3D11VertexShader> vs;
    assert(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
    const int params[4] = { N, 0, 0, 0 };
    D3D11_BUFFER_DESC bd = { sizeof(params), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { params, 0, 0 };
    ComPtr<ID3D11Buffer> constants;
    assert(SUCCEEDED(device->CreateBuffer(&bd, &init, &constants)));
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = N + 1; td.Height = 1; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)));
    ComPtr<ID3D11RenderTargetView> rtv;
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging)));

    auto knots = [&](const std::string& builder, const std::vector<float>& weights) {
        auto psCode = Compile(builder, "rtw_build", "ps_4_0");
        ComPtr<ID3D11PixelShader> ps;
        assert(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)));
        D3D11_TEXTURE2D_DESC id = {};
        id.Width = N; id.Height = 1; id.MipLevels = id.ArraySize = id.SampleDesc.Count = 1;
        id.Format = DXGI_FORMAT_R32_FLOAT; id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = { weights.data(), N * sizeof(float), 0 };
        ComPtr<ID3D11Texture2D> image;
        assert(SUCCEEDED(device->CreateTexture2D(&id, &data, &image)));
        ComPtr<ID3D11ShaderResourceView> view;
        assert(SUCCEEDED(device->CreateShaderResourceView(image.Get(), nullptr, &view)));
        ctx->ClearState();
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport = { 0, 0, float(N + 1), 1, 0, 1 };
        ctx->RSSetViewports(1, &viewport);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, view.GetAddressOf());
        ctx->Draw(3, 0);
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        std::vector<float> result((const float*)map.pData, (const float*)map.pData + N + 1);
        ctx->Unmap(staging.Get(), 0);
        return result;
    };

    // The important region reaches the map border: the view frustum's camera end.
    const std::vector<float> atBorder = { 0, 0, 0, 0, 1, 1, 1, 1 };
    auto vanilla = knots(source, atBorder);
    auto fixed = knots(patched, atBorder);
    assert(std::fabs(vanilla[N] - 1.05f) < 1e-6f);            // end knot pushed off the map
    assert(std::fabs(fixed[N] - 1.0f) < 1e-6f);               // end knot on the map edge
    for (int k = 0; k < N; ++k) assert(vanilla[k] == fixed[k]);
    for (int k = 4; k <= N; ++k) assert(fixed[k] >= -1.0f && fixed[k] <= 1.0f);   // whole region on the map

    // Away from the border only the region's end knot moves; everything beyond stays off-map.
    const std::vector<float> interior = { 0, 0, 1, 1, 1, 1, 0, 0 };
    vanilla = knots(source, interior);
    fixed = knots(patched, interior);
    for (int k = 0; k <= N; ++k) assert(k == 6 ? std::fabs(fixed[k] - 1.0f) < 1e-6f : vanilla[k] == fixed[k]);
    assert(fixed[7] > 1.0f && fixed[8] > 1.0f && fixed[1] < -1.0f);
    std::puts("RTW warp build: the region's end knot stays on the map, nothing else moves");
}
