// The effect shaders test for NaN/Inf with exponent-bit helpers instead of isfinite(),
// which vkd3d-shader (the HLSL compiler under Wine/Proton) does not define. Effect output
// cannot show a broken helper (smoothstep saturates NaN to 0 on D3D), so run the real
// helper files on WARP and compare every lane with std::isfinite on edge-case bit patterns.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

static const uint32_t kBits[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF,
    0x00800000, 0x00000001, 0x807FFFFF,             // finite, incl. denormals
    0x7F800000, 0xFF800000,                         // +/-Inf
    0x7FC00000, 0xFFC00000, 0x7F800001, 0xFFFFFFFF  // quiet, negative and signalling NaN
};
static const UINT kCount = sizeof(kBits) / sizeof(kBits[0]);

// Result bits per element i, with lanes x..w = input[i], input[i+1], input[i+2], input[i+3]:
// 0 RtgiIsFinite(x), 1-3 RtgiIsFinite3(xyz), 4-7 RtgiIsFinite4(xyzw), 8-10 OutlineIsFinite(xyz).
static const char* kShader =
    "#include \"../effects/rtgi/shaders/rtgi_finite.hlsl\"\n"
    "#include \"../effects/outline/shaders/outline_normals.hlsl\"\n"
    "StructuredBuffer<float> input : register(t0);\n"
    "RWStructuredBuffer<uint> result : register(u0);\n"
    "uint Pack(bool b, uint bit) { return b ? (1u << bit) : 0u; }\n"
    "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    uint n, stride; input.GetDimensions(n, stride);\n"
    "    float4 v = float4(input[id.x], input[(id.x + 1) % n], input[(id.x + 2) % n], input[(id.x + 3) % n]);\n"
    "    bool3 r3 = RtgiIsFinite3(v.xyz); bool4 r4 = RtgiIsFinite4(v); bool3 o3 = OutlineIsFinite(v.xyz);\n"
    "    uint bits = Pack(RtgiIsFinite(v.x), 0);\n"
    "    [unroll] for (uint c = 0; c < 3; ++c) bits |= Pack(r3[c], 1 + c) | Pack(o3[c], 8 + c);\n"
    "    [unroll] for (uint d = 0; d < 4; ++d) bits |= Pack(r4[d], 4 + d);\n"
    "    result[id.x] = bits;\n"
    "}\n";

static bool Finite(UINT i) { float f; std::memcpy(&f, &kBits[i % kCount], 4); return std::isfinite(f); }

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &ctx)));
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "finite_helpers.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(hr));
    ComPtr<ID3D11ComputeShader> cs;
    assert(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)));

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = kCount * 4; bd.StructureByteStride = 4;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { kBits };
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
    ctx->Dispatch(kCount, 1, 1);
    ctx->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    assert(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const uint32_t* got = (const uint32_t*)mapped.pData;
    for (UINT i = 0; i < kCount; ++i) {
        uint32_t expected = Finite(i) ? 1u : 0u;
        for (UINT c = 0; c < 3; ++c) expected |= (Finite(i + c) ? 1u : 0u) << (1 + c) | (Finite(i + c) ? 1u : 0u) << (8 + c);
        for (UINT d = 0; d < 4; ++d) expected |= (Finite(i + d) ? 1u : 0u) << (4 + d);
        if (got[i] != expected) {
            std::fprintf(stderr, "element %u (0x%08X): helpers 0x%03X, isfinite 0x%03X\n", i, kBits[i], got[i], expected);
            assert(false);
        }
    }
    ctx->Unmap(staging.Get(), 0);
    std::puts("RtgiIsFinite/3/4 and OutlineIsFinite match isfinite per lane for zeros, denormals, max, +/-Inf and NaNs");
}
