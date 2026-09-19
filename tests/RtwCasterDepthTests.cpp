#include "../src/RtwCasterDepth.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;

static ComPtr<ID3DBlob> TryCompile(const std::string& src, const char* entry, const char* target,
                                  const D3D_SHADER_MACRO* defines, ID3DInclude* includes, std::string* error = nullptr)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, defines, includes, entry, target,
        D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(hr) && errors && error) *error = (const char*)errors->GetBufferPointer();
    return SUCCEEDED(hr) ? code : nullptr;
}

static ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target,
                               const D3D_SHADER_MACRO* defines = nullptr, ID3DInclude* includes = nullptr)
{
    std::string error;
    auto code = TryCompile(src, entry, target, defines, includes, &error);
    if (!code) std::fprintf(stderr, "%s: %s", entry, error.c_str());
    assert(code);
    return code;
}

class FolderIncludes : public ID3DInclude {
    std::filesystem::path root;
public:
    explicit FolderIncludes(std::filesystem::path path) : root(std::move(path)) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        for (const auto& dir : {"common", "deferred"}) {
            std::ifstream file(root / dir / name, std::ios::binary);
            if (!file) continue;
            std::string source((std::istreambuf_iterator<char>(file)), {});
            char* copy = new char[source.size()];
            std::memcpy(copy, source.data(), source.size());
            *data = copy; *bytes = (UINT)source.size();
            return S_OK;
        }
        return E_FAIL;
    }
    HRESULT __stdcall Close(LPCVOID data) override { delete[] static_cast<const char*>(data); return S_OK; }
};

// The shape of the game's caster vertex shaders, without their source.
static const char* kCaster = R"(
cbuffer Test : register(b0) { float casterZ; };
float4 GetDistortedPosition(float4 position) { return position; }
void shadow_vs(uint id : SV_VertexID, out float4 outPos : SV_Position, out float depth : TEXCOORD0)
{
	float2 uv = float2((id << 1) & 2, id & 2);
	outPos = float4(uv * float2(2, -2) + float2(-1, 1), casterZ, 1);
	#ifdef RTW
	outPos = GetDistortedPosition(outPos);
	#endif

	outPos.z = max(outPos.z, 0.0); // avoid clipping
	depth = outPos.z;
}
float4 store_fs(float4 pixel : SV_Position, float depth : TEXCOORD0) : SV_Target { return depth; }
)";

static const char* kSkinned = R"(
float4 GetDistortedPosition(float4 position) { return position; }
void shadow_vs(float4 p : POSITION, out float4 oPosition : SV_Position, out float oDepth : TEXCOORD0)
{
	oPosition = GetDistortedPosition(p);
	oPosition.z = max(oPosition.z, 0.0f); // Avoid clipping
	oDepth = oPosition.z / oPosition.w;
}
)";

static const char* kTessellator = R"(
Texture2D warpMap : register(t0);
float4 GetDistortedPosition(Texture2D map, float4 position) { return position; }
void tessellator_vs(float4 p : POSITION, out float4 oPosition : SV_Position)
{
	oPosition = p;
	oPosition.z = max(oPosition.z, 0.0); // avoid clipping
}
struct HSOutput { float4 position : SV_Position; };
[domain("tri")]
void tessellator_ds(const OutputPatch<HSOutput, 3> polygon, float3 UVW : SV_DomainLocation,
	float edges[3] : SV_TessFactor, float inside : SV_InsideTessFactor,
	out float4 oPosition : SV_Position, out float oDepth : TEXCOORD0)
{
	float4 vertex = UVW.x * polygon[0].position + UVW.y * polygon[1].position + UVW.z * polygon[2].position;
	oPosition = GetDistortedPosition( warpMap, vertex );
	oDepth = oPosition.z;
}
)";

int main(int argc, char** argv)
{
    const D3D_SHADER_MACRO rtw[] = { {"RTW", "1"}, {nullptr, nullptr} };

    // Only caster entry points of RTW caster sources are touched, and only once.
    const std::string caster = kCaster;
    const std::string patched = RtwCasterDepth::Patch(caster, "shadow_vs");
    assert(patched != caster);
    assert(RtwCasterDepth::Patch(patched, "shadow_vs") == patched);
    assert(RtwCasterDepth::Patch(caster, "main_vs") == caster);
    assert(RtwCasterDepth::Patch("void shadow_vs() { p.z = max(p.z, 0.0);\n d = p.z; }", "shadow_vs") ==
           "void shadow_vs() { p.z = max(p.z, 0.0);\n d = p.z; }");   // no warp: not an RTW caster source

    // The divide-by-w and 0.0f spellings, and the tessellator's split clamp.
    Compile(RtwCasterDepth::Patch(kSkinned, "shadow_vs"), "shadow_vs", "vs_4_0", rtw);
    assert(RtwCasterDepth::Patch(kSkinned, "shadow_vs") != kSkinned);
    const std::string tess = RtwCasterDepth::Patch(kTessellator, "tessellator_ds");
    assert(tess != kTessellator && RtwCasterDepth::Patch(kTessellator, "tessellator_vs") == tess);
    Compile(tess, "tessellator_vs", "vs_5_0");
    Compile(tess, "tessellator_ds", "ds_5_0");
    assert(tess.find("oDepth = oPosition.z;\n\toPosition.z = max(") != std::string::npos);

    if (argc > 1)
    {
        const std::filesystem::path materials = argv[1];
        FolderIncludes includes(materials);
        struct Entry { const char* file; const char* entry; const char* target; bool rtwDefine; };
        const Entry entries[] = {
            {"common/shadowcaster.hlsl", "shadow_vs", "vs_4_0", true},
            {"common/rtwshadows.hlsl", "rtw_shadow_vs", "vs_4_0", false},
            {"common/rtwtessellator.hlsl", "tessellator_vs", "vs_5_0", false},
            {"common/rtwtessellator.hlsl", "tessellator_ds", "ds_5_0", false},
            {"deferred/skin.hlsl", "shadow_vs", "vs_4_0", true},
            {"deferred/terrain.hlsl", "shadow_vs", "vs_4_0", true},
            {"deferred/terrainfp4.hlsl", "terrain_shadow_vs", "vs_4_0", true},
            {"deferred/foliage.hlsl", "farm_shadow_vs", "vs_4_0", true},
            {"deferred/construction.hlsl", "construction_shadow_vs", "vs_4_0", true},
            {"deferred/construction.hlsl", "foliage_shadow_vs", "vs_4_0", true},
        };
        int compiled = 0;
        for (const Entry& e : entries)
        {
            std::ifstream file(materials / e.file);
            assert(file.good());
            std::string source((std::istreambuf_iterator<char>(file)), {});
            std::string gamePatched = RtwCasterDepth::Patch(source, e.entry);
            if (gamePatched == source) std::fprintf(stderr, "not patched: %s %s\n", e.file, e.entry);
            assert(gamePatched != source);
            // The game builds some of these with its own per-program setup. Whatever
            // compiles here unpatched must still compile patched.
            for (const D3D_SHADER_MACRO* defines : {e.rtwDefine ? rtw : nullptr, (const D3D_SHADER_MACRO*)nullptr})
            {
                if (!TryCompile(source, e.entry, e.target, defines, &includes)) continue;
                Compile(gamePatched, e.entry, e.target, defines, &includes);
                ++compiled;
            }
        }
        std::printf("Installed game: 10 RTW caster entry points patched, %d variants compiled\n", compiled);
        assert(compiled >= 10);
    }

    // Render a caster 0.5 depth units SUNWARD of the near plane into an R32F map.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &ctx)));
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)));
    ComPtr<ID3D11RenderTargetView> rtv;
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging)));
    const float casterZ[4] = { -0.5f, 0, 0, 0 };
    D3D11_BUFFER_DESC bd = { sizeof(casterZ), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { casterZ, 0, 0 };
    ComPtr<ID3D11Buffer> constants;
    assert(SUCCEEDED(device->CreateBuffer(&bd, &init, &constants)));
    auto psCode = Compile(caster, "store_fs", "ps_4_0");
    ComPtr<ID3D11PixelShader> ps;
    assert(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)));

    auto stored = [&](const std::string& source, const D3D_SHADER_MACRO* defines) {
        auto vsCode = Compile(source, "shadow_vs", "vs_4_0", defines);
        ComPtr<ID3D11VertexShader> vs;
        assert(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
        const float clear[4] = { 1, 1, 1, 1 };
        ctx->ClearState();
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport = { 0, 0, 8, 8, 0, 1 };
        ctx->RSSetViewports(1, &viewport);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->Draw(3, 0);
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        float value = static_cast<const float*>(map.pData)[0];
        ctx->Unmap(staging.Get(), 0);
        return value;
    };

    assert(stored(caster, rtw) == 0.0f);                       // vanilla: stored at the near plane
    assert(std::fabs(stored(patched, rtw) + 0.5f) < 1e-6f);    // patched: true depth, still rasterised
    assert(stored(patched, nullptr) == 0.0f);                  // cascade variants are unchanged
    std::puts("RTW caster depth: casters sunward of the near plane store their true depth and still rasterise");
}
