#include "EffectShaderProbe.h"
#include <limits>

// OutlineIsFinite replaces isfinite(), which vkd3d-shader (Proton) lacks. The
// effect output cannot show the difference (smoothstep saturates NaN to 0 on
// D3D), so compare the helper itself with std::isfinite on edge-case bit patterns.
static void TestOutlineIsFinite(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    const uint32_t bits[] = {
        0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF,
        0x00800000, 0x00000001, 0x807FFFFF,             // finite, incl. denormals
        0x7F800000, 0xFF800000,                         // +/-Inf
        0x7FC00000, 0xFFC00000, 0x7F800001, 0xFFFFFFFF  // quiet, negative and signalling NaN
    };
    const UINT count = sizeof(bits) / sizeof(bits[0]);
    const char* src =
        "#include \"../effects/outline/shaders/outline_normals.hlsl\"\n"
        "StructuredBuffer<float> input : register(t0);\n"
        "RWStructuredBuffer<uint> result : register(u0);\n"
        "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
        "    result[id.x] = OutlineIsFinite(input[id.x].xxx).x ? 1 : 0;\n"
        "}\n";
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src, std::strlen(src), "outline_is_finite.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(hr));
    ComPtr<ID3D11ComputeShader> cs;
    assert(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)));

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = count * 4; bd.StructureByteStride = 4;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { bits };
    ComPtr<ID3D11Buffer> in, out, staging;
    assert(SUCCEEDED(device->CreateBuffer(&bd, &init, &in)));
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    assert(SUCCEEDED(device->CreateBuffer(&bd, nullptr, &out)));
    bd.BindFlags = 0; bd.MiscFlags = 0; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateBuffer(&bd, nullptr, &staging)));
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    assert(SUCCEEDED(device->CreateShaderResourceView(in.Get(), nullptr, &srv)));
    assert(SUCCEEDED(device->CreateUnorderedAccessView(out.Get(), nullptr, &uav)));

    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    ctx->Dispatch(count, 1, 1);
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ctx->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const uint32_t* got = (const uint32_t*)mapped.pData;
    for (UINT i = 0; i < count; ++i) {
        float value; std::memcpy(&value, &bits[i], 4);
        const uint32_t expected = std::isfinite(value) ? 1u : 0u;
        if (got[i] != expected) {
            std::fprintf(stderr, "OutlineIsFinite(0x%08X) = %u, isfinite = %u\n", bits[i], got[i], expected);
            assert(false);
        }
    }
    ctx->Unmap(staging.Get(), 0);
}

int main() {
    EffectShaderProbe p("outline", "DustOutline.dll");
    TestOutlineIsFinite(p.device.Get(), p.ctx.Get());
    const std::vector<Pixel> scene(p.W*p.H, Pixel{.7f,.6f,.5f,1});
    const std::vector<Pixel> black(p.W*p.H, Pixel{0,0,0,1});
    // Skinned shaders can store non-unit normals. A constant short normal
    // is a flat surface, not a normal discontinuity covering the entire mesh.
    std::vector<Pixel> normals(p.W*p.H, Pixel{.5f,.5f,.65f,1});
    p.SetNormals(normals);
    p.Setting<bool>("DebugView") = true;
    AssertImagesNear(p.Render(scene,.05f), black);
    p.Setting<bool>("DebugView") = false;
    AssertImagesNear(p.Render(scene,.05f), scene);
    // Vary length alone: the angle remains constant, including 8-bit encoding.
    for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x)
        normals[y*p.W+x] = {.5f,.5f,(x%2 ? 166.f : 230.f)/255.f,1};
    p.SetNormals(normals); p.Setting<bool>("DebugView") = true;
    AssertImagesNear(p.Render(scene,.05f), black);
    // A real 90-degree crease still draws one-pixel green edges.
    for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x)
        normals[y*p.W+x] = x < p.W/2 ? Pixel{.5f,.5f,.65f,1} : Pixel{.9f,.5f,.5f,1};
    p.SetNormals(normals);
    auto expected = black;
    for (UINT y = 0; y < p.H; ++y) expected[y*p.W+p.W/2-1][1] = 1;
    AssertImagesNear(p.Render(scene,.05f), expected);
    p.Setting<bool>("DebugView") = false;
    expected = scene;
    for (UINT y = 0; y < p.H; ++y) for (int c = 0; c < 3; ++c)
        expected[y*p.W+p.W/2-1][c] *= 1 - p.Setting<float>("Strength");
    AssertImagesNear(p.Render(scene,.05f), expected);
    p.Setting<bool>("DebugView") = true;
    AssertImagesNear(p.Render(scene,.6f), black); // debug obeys distance exclusion
    p.SetNormals(std::vector<Pixel>(p.W*p.H, Pixel{128.f/255,128.f/255,128.f/255,1}));
    AssertImagesNear(p.Render(scene,.05f), black); // neutral/degenerate GBuffer normals
    // NaN and +/-Inf normals leave flat surfaces unoutlined and never reach the output.
    const float inf = std::numeric_limits<float>::infinity();
    normals.assign(p.W*p.H, Pixel{.5f,.5f,.65f,1});
    normals[p.W+1] = {std::numeric_limits<float>::quiet_NaN(),.5f,.65f,1};
    normals[2*p.W+3] = {.5f,inf,.65f,1};
    normals[3*p.W+5] = {.5f,.5f,-inf,1};
    p.SetNormals(normals); p.Setting<bool>("DebugView") = false;
    AssertImagesNear(p.Render(scene,.05f), scene);
    p.Setting<bool>("DebugView") = true; // the checks below expect the debug view
    AssertImagesNear(p.Render(scene,.05f), black);
    p.hasNormals = false;
    AssertImagesNear(p.Render(scene,.05f), black);
    std::puts("OutlineIsFinite matches isfinite; short/variable/invalid/non-finite normals preserve flat surfaces; real creases remain outlined in both paths");
}
