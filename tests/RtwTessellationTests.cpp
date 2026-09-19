#include "../src/RtwTessellation.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;

static ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(hr));
    return code;
}

// Same interface and edge metric as the game's tessellator, without its source.
static const char* kTessellator = R"(
struct VSOutput { float4 position : SV_Position; float4 p2d : TEXCOORD0; float4 texcoord : TEXCOORD1; };
struct HSOutput { float4 position : SV_Position; float4 texcoord : TEXCOORD0; float conValue : TEXCOORD1; };

void constant_hs (
	InputPatch<VSOutput,3> patch,
	out float edges[3] : SV_TessFactor,
	out float inside : SV_InsideTessFactor
) {
	float max_edge_len = 0.1;
	edges[0] = floor( 1+length( patch[1].p2d.xy - patch[2].p2d.xy ) / max_edge_len );
	edges[1] = floor( 1+length( patch[2].p2d.xy - patch[0].p2d.xy ) / max_edge_len );
	edges[2] = floor( 1+length( patch[0].p2d.xy - patch[1].p2d.xy ) / max_edge_len );
	inside   = floor( 1+(edges[0]+edges[1]+edges[2])/3.0f );
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("constant_hs")]
HSOutput tessellator_hs (InputPatch<VSOutput,3> patch, uint i : SV_OutputControlPointID)
{
	HSOutput output;
	output.position = patch[i].position;
	output.texcoord = patch[i].texcoord;
	output.conValue = patch[i].p2d.w;
	return output;
}
)";

static const char* kStages = R"(
cbuffer Patch : register(b0) { float4 positions[3]; float4 warped[3]; };
struct VSOutput { float4 position : SV_Position; float4 p2d : TEXCOORD0; float4 texcoord : TEXCOORD1; };
struct HSOutput { float4 position : SV_Position; float4 texcoord : TEXCOORD0; float conValue : TEXCOORD1; };
VSOutput vs(uint id : SV_VertexID)
{
	VSOutput o;
	o.position = positions[id];
	o.p2d = warped[id];
	o.texcoord = 0;
	return o;
}
[domain("tri")]
float4 ds(const OutputPatch<HSOutput, 3> p, float3 uvw : SV_DomainLocation,
          float edges[3] : SV_TessFactor, float inside : SV_InsideTessFactor) : SV_Position
{
	return uvw.x * p[0].position + uvw.y * p[1].position + uvw.z * p[2].position;
}
)";

struct PatchData { float positions[3][4]; float warped[3][4]; };

// The same hull shader with literal factors: what the tessellator emits for them.
static std::string FixedFactors(unsigned e0, unsigned e1, unsigned e2, unsigned inside)
{
    std::string source = kTessellator;
    size_t from = source.find("float max_edge_len");
    size_t to = source.find(';', source.find("inside   ="));
    source.replace(from, to + 1 - from,
        "edges[0] = " + std::to_string(e0) + "; edges[1] = " + std::to_string(e1) +
        "; edges[2] = " + std::to_string(e2) + "; inside = " + std::to_string(inside) + ";");
    return source;
}

int main(int argc, char** argv)
{
    const std::string source = kTessellator;
    const std::string patched = RtwTessellation::Patch(source);
    assert(patched != source);
    assert(RtwTessellation::Patch(patched) == patched);          // applied once
    assert(RtwTessellation::Patch("float4 main() : SV_Target { return 0; }") ==
           "float4 main() : SV_Target { return 0; }");            // unrelated sources untouched
    assert(patched.find("max_edge_len") == std::string::npos);

    if (argc > 1)
    {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string game((std::istreambuf_iterator<char>(file)), {});
        std::string gamePatched = RtwTessellation::Patch(game);
        assert(gamePatched != game);
        Compile(gamePatched, "tessellator_hs", "hs_5_0");
        std::puts("Real game rtwtessellator.hlsl: patched hull shader compiles");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &device, nullptr, &ctx)));

    auto vsCode = Compile(kStages, "vs", "vs_5_0");
    auto dsCode = Compile(kStages, "ds", "ds_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11DomainShader> ds;
    assert(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
    assert(SUCCEEDED(device->CreateDomainShader(dsCode->GetBufferPointer(), dsCode->GetBufferSize(), nullptr, &ds)));
    const D3D11_SO_DECLARATION_ENTRY decl[] = { {0, "SV_Position", 0, 0, 4, 0} };
    const UINT stride = 16;
    ComPtr<ID3D11GeometryShader> streamOut;
    assert(SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(dsCode->GetBufferPointer(),
        dsCode->GetBufferSize(), decl, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &streamOut)));

    D3D11_BUFFER_DESC cbDesc = { sizeof(PatchData), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ComPtr<ID3D11Buffer> constants;
    assert(SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, &constants)));
    D3D11_BUFFER_DESC soDesc = { 64 * 64 * 3 * stride * 2, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0 };
    ComPtr<ID3D11Buffer> soBuffer;
    assert(SUCCEEDED(device->CreateBuffer(&soDesc, nullptr, &soBuffer)));
    D3D11_QUERY_DESC queryDesc = { D3D11_QUERY_SO_STATISTICS, 0 };
    ComPtr<ID3D11Query> query;
    assert(SUCCEEDED(device->CreateQuery(&queryDesc, &query)));

    auto triangles = [&](const std::string& hullSource, const PatchData& data) {
        auto hsCode = Compile(hullSource, "tessellator_hs", "hs_5_0");
        ComPtr<ID3D11HullShader> hs;
        assert(SUCCEEDED(device->CreateHullShader(hsCode->GetBufferPointer(), hsCode->GetBufferSize(), nullptr, &hs)));
        ctx->ClearState();
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
        ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->HSSetShader(hs.Get(), nullptr, 0);
        ctx->DSSetShader(ds.Get(), nullptr, 0);
        ctx->GSSetShader(streamOut.Get(), nullptr, 0);
        UINT offset = 0;
        ctx->SOSetTargets(1, soBuffer.GetAddressOf(), &offset);
        ctx->Begin(query.Get());
        ctx->Draw(3, 0);
        ctx->End(query.Get());
        D3D11_QUERY_DATA_SO_STATISTICS stats = {};
        while (ctx->GetData(query.Get(), &stats, sizeof(stats), 0) == S_FALSE) {}
        return (unsigned)stats.PrimitivesStorageNeeded;
    };

    const float cell = 2.0f / 513.0f;   // one warp cell in clip units

    // A small on-map triangle is left alone. The vanilla metric splits it anyway.
    PatchData tiny = { { {0, 0, 0.5f, 1}, {0.1f * cell, 0, 0.5f, 1}, {0, 0.1f * cell, 0.5f, 1} },
                        { {0, 0, 0, 0}, {0.01f, 0, 0, 0}, {0, 0.01f, 0, 0} } };
    assert(triangles(source, tiny) == triangles(FixedFactors(1, 1, 1, 2), tiny));
    assert(triangles(patched, tiny) == 1);

    // Both ends of every edge warp to almost the same place (collapsed zones), but the
    // edges cross just under ten warp cells. The vanilla metric does not subdivide them at all.
    PatchData crossing = { { {-4.95f * cell, 0, 0.5f, 1}, {4.95f * cell, 0, 0.5f, 1}, {0, 9.9f * cell, 0.5f, 1} },
                           { {0.90f, 0, 0, 0}, {0.91f, 0, 0, 0}, {0.905f, 0.01f, 0, 0} } };
    const unsigned n = 10 * RtwTessellation::SegmentsPerCell;
    assert(triangles(source, crossing) == triangles(FixedFactors(1, 1, 1, 2), crossing));
    assert(triangles(patched, crossing) == triangles(FixedFactors(n, n, n, n), crossing));

    // The same patch entirely beyond the map border is never rasterised: no work.
    PatchData offMap = crossing;
    for (auto& w : offMap.warped) w[0] += 0.2f;
    assert(triangles(patched, offMap) == 1);

    // Factors never exceed the hardware limit.
    PatchData huge = { { {-0.9f, -0.9f, 0.5f, 1}, {0.9f, -0.9f, 0.5f, 1}, {0, 0.9f, 0.5f, 1} },
                       { {-0.5f, -0.5f, 0, 0}, {0.5f, -0.5f, 0, 0}, {0, 0.5f, 0, 0} } };
    assert(triangles(patched, huge) == triangles(FixedFactors(64, 64, 64, 64), huge));

    std::puts("RTW tessellation: factors follow warp cells crossed, skip off-map and small patches, cap at 64");
}
