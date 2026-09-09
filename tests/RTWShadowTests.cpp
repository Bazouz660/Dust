#include "EffectShaderProbe.h"
#include "../effects/shadows/RTWShadowShader.h"
#include <algorithm>
#include <fstream>
#include <iterator>

static void Log(const char*, ...) {}
#include "build/DeferredShader.generated.h"

static ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry,
                              const char* target, ID3DInclude* includes = nullptr,
                              const D3D_SHADER_MACRO* defines = nullptr)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(source.data(), source.size(), nullptr, defines, includes,
        entry, target, D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &errors);
    if (FAILED(result) && errors) std::fprintf(stderr, "%s", (const char*)errors->GetBufferPointer());
    assert(SUCCEEDED(result));
    return code;
}

// Optional compilation against the installed game; proprietary sources stay out of git.
class GameIncludes : public ID3DInclude {
    std::filesystem::path materials;
public:
    explicit GameIncludes(const char* path) : materials(path) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        for (const auto& dir : {"common", "deferred"}) {
            std::ifstream file(materials / dir / name, std::ios::binary);
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

struct ShadowParams {
    float enabled = 1, filterRadius = .01f, lightSize = .03f, pcss = 0;
    float cliffFix = 0, cliffDistance = .1f, csmRadius = 1, csmBlend = 1;
    float csmWidth = .15f, quality = 12, texel = 1.f / 256, csmFar = .85f;
};

struct SceneParams {
    float slope = .4f, warpScale = 1, depthOffset = .1f, depthScale = .1f;
    float projectionScale = .1f, receiverDepth = 3, blockerGap = 0, padding = 0;
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc > 1) {
        auto path = std::filesystem::path(argv[1]) / "deferred/deferred.hlsl";
        std::ifstream file(path);
        assert(file.good());
        std::string source((std::istreambuf_iterator<char>(file)), {});
        GameIncludes includes(argv[1]);
        for (const char* mode : {"RTW", "CSM", "NOSHADOW"}) {
            const D3D_SHADER_MACRO defines[] = {{mode, "1"}, {nullptr, nullptr}};
            for (bool workshop : {false, true}) {
                auto patched = PatchDeferredShader(source + (workshop ? "\n// steepBias\n" : ""));
                assert(patched.find("DustRtwReceiverGradient") != std::string::npos);
                Compile(patched, "main_fs", "ps_4_0", &includes, defines);
            }
        }
        std::puts("Installed deferred shader: RTW/CSM/no-shadow and workshop variants compile");
    }

    // Obtain the exact injected RTW code and b7 layout from the production patcher.
    auto patched = PatchDeferredShader("void main_vs (\n// uniform float4 ambientParams,\n"
                                       "// LightingData ld = (LightingData)0.0f;\n");
    auto begin = patched.find("cbuffer DustShadowParams");
    auto end = patched.find("static const float2 kDustCsmPoisson", begin);
    assert(begin != std::string::npos && end != std::string::npos);
    std::string source = patched.substr(begin, end - begin) + R"hlsl(
cbuffer Scene : register(b0) {
    float slope, warpScale, depthOffset, depthScale;
    float projectionScale, receiverDepth, blockerGap, padding;
};
sampler2D depthMap : register(s0);
sampler2D warpMap : register(s1);
float4 main(float4 pixel : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 xy = (uv - .5) * .2 / projectionScale;
    float3 world = float3(xy, receiverDepth + xy.x * slope);
    float4x4 shadowProjection = float4x4(projectionScale, 0, 0, .5,
                               0, projectionScale, 0, .5,
                               0, 0, depthScale, depthOffset,
                               0, 0, 0, 1);
    float visibility = DustRTWShadow(depthMap, warpMap, shadowProjection, world, .00003, 0,
                                    pixel.xy, normalize(float3(-slope, 0, 1)), 0, 100);
    return visibility.xxxx;
})hlsl";
    EffectShaderProbe probe("ssao", "DustSSAO.dll");
    auto pixelCode = Compile(source, "main", "ps_4_0");
    // Legacy sampler registers fix sN, but D3DCompile assigns tN separately.
    ComPtr<ID3D11ShaderReflection> reflection;
    assert(SUCCEEDED(D3DReflect(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), IID_PPV_ARGS(&reflection))));
    D3D11_SHADER_DESC shaderDesc;
    reflection->GetDesc(&shaderDesc);
    UINT depthSlot = UINT_MAX, warpSlot = UINT_MAX;
    for (UINT i = 0; i < shaderDesc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC resource;
        reflection->GetResourceBindingDesc(i, &resource);
        if (resource.Type != D3D_SIT_TEXTURE) continue;
        if (!std::strcmp(resource.Name, "depthMap")) depthSlot = resource.BindPoint;
        if (!std::strcmp(resource.Name, "warpMap")) warpSlot = resource.BindPoint;
    }
    assert(depthSlot != UINT_MAX && warpSlot != UINT_MAX);
    ComPtr<ID3D11PixelShader> pixelShader;
    assert(SUCCEEDED(probe.device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixelShader)));
    ComPtr<ID3DBlob> vertexCode;
    vertexCode.Attach(EffectShaderProbe::Compile("fullscreen_vs.hlsl", "main", "vs_4_0"));
    ComPtr<ID3D11VertexShader> vertexShader;
    assert(SUCCEEDED(probe.device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertexShader)));
    ComPtr<ID3D11Buffer> shadowBuffer, sceneBuffer;
    shadowBuffer.Attach(probe.host.CreateConstantBuffer(probe.device.Get(), sizeof(ShadowParams)));
    sceneBuffer.Attach(probe.host.CreateConstantBuffer(probe.device.Get(), sizeof(SceneParams)));
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    assert(SUCCEEDED(probe.device->CreateSamplerState(&samplerDesc, &sampler)));

    auto texture = [&](UINT width, UINT height, const std::vector<float>& values) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = {values.data(), width * sizeof(float), 0};
        ComPtr<ID3D11Texture2D> image;
        assert(SUCCEEDED(probe.device->CreateTexture2D(&desc, &data, &image)));
        ComPtr<ID3D11ShaderResourceView> view;
        assert(SUCCEEDED(probe.device->CreateShaderResourceView(image.Get(), nullptr, &view)));
        return view;
    };

    auto render = [&](const SceneParams& scene, ShadowParams shadows, UINT resolution, bool edge = false) {
        shadows.texel = 1.f / resolution;
        std::vector<float> depths(resolution * resolution);
        for (UINT y = 0; y < resolution; ++y) for (UINT x = 0; x < resolution; ++x) {
            float worldX = ((x + .5f) / resolution - .5f) / (scene.warpScale * scene.projectionScale);
            float gap = (!edge || worldX < 0) ? scene.blockerGap : 0;
            depths[y * resolution + x] = scene.depthOffset + scene.depthScale * (scene.receiverDepth + scene.slope * worldX - gap);
        }
        std::vector<float> warp(513 * 2);
        for (UINT i = 0; i < warp.size(); ++i)
            warp[i] = (scene.warpScale - 1) * ((i % 513 + .5f) / 513 - .5f);
        auto depthView = texture(resolution, resolution, depths);
        auto warpView = texture(513, 2, warp);
        probe.ctx->ClearState();
        probe.host.UpdateConstantBuffer(probe.ctx.Get(), sceneBuffer.Get(), &scene, sizeof(scene));
        probe.host.UpdateConstantBuffer(probe.ctx.Get(), shadowBuffer.Get(), &shadows, sizeof(shadows));
        probe.ctx->PSSetConstantBuffers(0, 1, sceneBuffer.GetAddressOf());
        probe.ctx->PSSetConstantBuffers(7, 1, shadowBuffer.GetAddressOf());
        ID3D11SamplerState* samplers[] = {sampler.Get(), sampler.Get()};
        probe.ctx->PSSetShaderResources(depthSlot, 1, depthView.GetAddressOf());
        probe.ctx->PSSetShaderResources(warpSlot, 1, warpView.GetAddressOf());
        probe.ctx->PSSetSamplers(0, 2, samplers);
        probe.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        probe.ctx->VSSetShader(vertexShader.Get(), nullptr, 0);
        probe.ctx->PSSetShader(pixelShader.Get(), nullptr, 0);
        probe.ctx->OMSetRenderTargets(1, probe.outputRTV.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport = {0, 0, float(probe.W), float(probe.H), 0, 1};
        probe.ctx->RSSetViewports(1, &viewport);
        probe.ctx->Draw(3, 0);
        probe.ctx->CopyResource(probe.staging.Get(), probe.output.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        assert(SUCCEEDED(probe.ctx->Map(probe.staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        std::vector<Pixel> image(probe.W * probe.H);
        for (UINT y = 0; y < probe.H; ++y)
            std::memcpy(image.data() + y * probe.W, (char*)map.pData + y * map.RowPitch, probe.W * sizeof(Pixel));
        probe.ctx->Unmap(probe.staging.Get(), 0);
        return image;
    };

    for (float slope : {-.8f, 0.f, .8f}) for (float warp : {.5f, 1.f, 2.f})
    for (UINT size : {128u, 512u, 2048u}) for (float pcss : {0.f, 1.f}) {
        SceneParams scene; scene.slope = slope; scene.warpScale = warp;
        ShadowParams shadows; shadows.pcss = pcss;
        auto lit = render(scene, shadows, size);
        for (const auto& pixel : lit) {
            if (pixel[0] != 1.f) std::fprintf(stderr, "acne: slope=%g warp=%g size=%u pcss=%g visibility=%g\n", slope, warp, size, pcss, pixel[0]);
            assert(pixel[0] == 1.f);
        }
        scene.blockerGap = .05f;
        auto occluded = render(scene, shadows, size);
        for (const auto& pixel : occluded) assert(pixel[0] == 0.f);
    }
    std::puts("RTW: sloped receivers remain lit and nearby blockers remain shadowed across warp, resolution and PCSS modes");
}
