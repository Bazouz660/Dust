#include "RtSystem.h"
#include "RtCore.h"
#include "RtScene.h"
#include "RtLog.h"
#include "D3D11Hook.h"
#include "D3D11StateBlock.h"
#include "CSMCapture.h"
#include "ShaderPatch.h"
#include "SceneAccess.h"
#include "TerrainTess.h"
#include "GeometryReplay.h"
#include "DustAPI.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <string>

namespace RtSystem
{

static Settings sSettings;
Settings& GetSettings() { return sSettings; }

enum class InitState : uint8_t { Uninit, Ready, Failed };
static InitState sInit = InitState::Uninit;
static char sStatus[256] = "waiting for first frame";

// must match the cbuffer in rt_common.hlsli (15 float4)
struct RtConstants
{
    float camRight[4], camUp[4], camForward[4], camPos[4];
    float prevCamRight[4], prevCamUp[4], prevCamForward[4], prevCamPos[4];
    float sunDir[4], sunColor[4], skyHorizon[4], skyZenith[4];
    float giParams[4], misc[4], res[4];
};

// ---- D3D12 side -----------------------------------------------------------
static ID3D12RootSignature* sRootSig = nullptr;
static ID3D12PipelineState* sPsoTrace = nullptr;
static ID3D12PipelineState* sPsoTemporal = nullptr;
static ID3D12PipelineState* sPsoAtrous = nullptr;
static ID3D12PipelineState* sPsoDebug = nullptr;
static IUnknown* sShaderBlobs[4] = {};   // keep DXIL alive (PSO creation copies, but cheap insurance)

static RtCore::SharedTexture sDepthSh;      // game depth copy   (11 -> 12)
static RtCore::SharedTexture sNormalsSh;    // game normals copy (11 -> 12)
static RtCore::SharedTexture sGiOutSh;      // filtered GI + sun vis (12 -> 11)
static RtCore::SharedTexture sDebugSh;      // clay view             (12 -> 11)
static DXGI_FORMAT sDepthSrcFmt = DXGI_FORMAT_UNKNOWN;
static DXGI_FORMAT sNormalsSrcFmt = DXGI_FORMAT_UNKNOWN;

static ID3D12Resource* sGiRaw = nullptr;        // RGBA16F
static ID3D12Resource* sAccum[2] = {};          // RGBA16F ping/pong history
static ID3D12Resource* sAtrousTmp[2] = {};      // RGBA16F scratch
static ID3D12Resource* sPrevDepth = nullptr;    // R32F
static ID3D12Resource* sDebugDepth = nullptr;   // R32F  (self-test GBuffer synth)
static ID3D12Resource* sDebugNorm = nullptr;    // RGBA16F
static int sAccumPing = 0;

static uint32_t sWidth = 0, sHeight = 0;
static bool sHistoryValid = false;
static RtConstants sPrevFrameCam = {};
static bool sPrevCamCaptured = false;

static uint64_t sTraceFence = 0;
static bool sFrameTraced = false;
static bool sSunValid = false;
static int sSunSource = 0;   // 0 = fallback, 1 = CSM snapshot, 2 = scene directional light
static uint64_t sFrameCounter = 0;

// Offline-replay snapshot: dumped once automatically when a real scene is up
// (or on the GUI button). Consumed by RtSelfTest --replay for offline debugging.
static bool sDumpRequested = false;
static bool sAutoDumpDone = false;
static float sLastIv[16] = {};
static float sLastProj[16] = {};

// TLAS-vs-GBuffer alignment instrumentation (active in clay debug mode):
// 8-byte counter buffer written by rt_debug_cs, copied to a readback ring.
static ID3D12Resource* sAlignBuf = nullptr;
static ID3D12Resource* sAlignZeros = nullptr;
static ID3D12Resource* sAlignReadback[3] = {};
static uint64_t        sAlignFence[3] = {};
static float           sAlignPct = -1.0f;   // -1 = no measurement

// ---- D3D11 composite side ---------------------------------------------------
static ID3D11ShaderResourceView* sGiOutSRV11 = nullptr;
static ID3D11ShaderResourceView* sDebugSRV11 = nullptr;
static ID3D11VertexShader* sFsVS = nullptr;
static ID3D11PixelShader*  sCompositePS = nullptr;
static ID3D11PixelShader*  sDebugPS = nullptr;
static ID3D11BlendState*   sAddBlend = nullptr;
static ID3D11BlendState*   sOpaqueBlend = nullptr;
static ID3D11DepthStencilState* sNoDepth = nullptr;
static ID3D11RasterizerState*   sNoCull = nullptr;
static ID3D11SamplerState* sLinearSamp = nullptr;
static ID3D11Buffer* sCompositeCB = nullptr;
static ID3D11Buffer* sRtParamsCB = nullptr;   // b9 for the patched main_fs
static bool sFlushRequested = false;

static void SetStatus(const char* msg)
{
    strncpy_s(sStatus, msg, _TRUNCATE);
    Log("RtSystem: %s", msg);
}
const char* StatusString() { return sStatus; }
bool IsActive() { return sInit == InitState::Ready && sSettings.enabled; }
void RequestFlush() { sFlushRequested = true; }
void RequestDump() { sDumpRequested = true; }

FrameStats GetFrameStats()
{
    FrameStats fs;
    RtScene::Stats s = RtScene::GetStats();
    fs.instances = s.instances;
    fs.meshes = s.meshes;
    fs.blasBuilt = s.blasBuilt;
    fs.pendingPairs = s.pendingPairs;
    fs.skippedInstanced = s.skippedInstanced;
    fs.parseFailures = s.parseFailures;
    fs.poolVerts = s.poolVertsUsed;
    fs.poolIdx = s.poolIdxUsed;
    fs.traced = sFrameTraced;
    fs.sunValid = sSunValid;
    fs.sunSource = sSunSource;
    fs.mainFsPatched = ShaderPatch::IsRtMainFsPatched();
    fs.tlasAlignPct = sAlignPct;
    return fs;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
static std::wstring Widen(const std::string& s)
{
    std::wstring w(s.begin(), s.end());   // mod paths are ASCII
    return w;
}

static bool CompileShaders()
{
    std::wstring dir = Widen(DustLogDir());          // mod dir, trailing slash
    std::wstring rtDir = dir + L"rt\\";

    if (!RtCore::InitShaderCompiler(dir.c_str()))    // dxcompiler.dll / dxil.dll next to Dust.dll
        return false;

    sRootSig = RtCore::CreateCommonRootSig(6, 4);
    if (!sRootSig) return false;

    struct ShaderDef { const wchar_t* file; ID3D12PipelineState** pso; };
    const std::wstring files[4] = {
        rtDir + L"rt_trace_cs.hlsl", rtDir + L"rt_temporal_cs.hlsl",
        rtDir + L"rt_atrous_cs.hlsl", rtDir + L"rt_debug_cs.hlsl"
    };
    ID3D12PipelineState** psos[4] = { &sPsoTrace, &sPsoTemporal, &sPsoAtrous, &sPsoDebug };

    for (int i = 0; i < 4; ++i)
    {
        const void* bc = nullptr;
        size_t bcSize = 0;
        IUnknown* blob = RtCore::CompileCS(files[i].c_str(), rtDir.c_str(), &bc, &bcSize);
        if (!blob) return false;
        *psos[i] = RtCore::CreateComputePSO(sRootSig, bc, bcSize);
        sShaderBlobs[i] = blob;
        if (!*psos[i]) return false;
    }
    return true;
}

static const char* kFsVsSrc =
    "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
    "    uv = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

// Kenshi's GBuffer albedo is YCoCg, chroma checkerboarded into buf0.g (B/A hold
// metalness/gloss) — faithful port of decodePixel/edgeFilterChroma/toRGB from
// data/materials/deferred/gbuffer.hlsl. Raw .rgb sampling tints everything green.
static const char* kCompositePsSrc =
    "Texture2D giTex : register(t0);\n"
    "Texture2D albedoTex : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer P : register(b0) { float4 p; }   // x = intensity\n"
    "float3 ToRGB(float3 c) {\n"
    "    c.y -= 0.5; c.z -= 0.5;\n"
    "    return float3(c.r + c.g - c.b, c.r + c.b, c.r - c.g - c.b);\n"
    "}\n"
    "float3 DecodeAlbedo(int2 px) {\n"
    "    float2 c  = albedoTex.Load(int3(px, 0)).rg;\n"
    "    float2 a0 = albedoTex.Load(int3(px + int2(1, 0), 0)).rg;\n"
    "    float2 a1 = albedoTex.Load(int3(px - int2(1, 0), 0)).rg;\n"
    "    float2 a2 = albedoTex.Load(int3(px + int2(0, 1), 0)).rg;\n"
    "    float2 a3 = albedoTex.Load(int3(px - int2(0, 1), 0)).rg;\n"
    "    float4 lum = float4(a0.r, a1.r, a2.r, a3.r);\n"
    "    float t = 30.0 / 255.0;\n"
    "    float4 w = 1.0 - step(float4(t, t, t, t), abs(lum - c.r));\n"
    "    float W = w.x + w.y + w.z + w.w;\n"
    "    w.x = (W == 0.0) ? 1.0 : w.x;\n"
    "    W   = (W == 0.0) ? 1.0 : W;\n"
    "    float chroma2 = (w.x * a0.g + w.y * a1.g + w.z * a2.g + w.w * a3.g) / W;\n"
    "    float3 col = float3(c.r, c.g, chroma2);\n"
    "    col = ((px.x & 1) == (px.y & 1)) ? col.rbg : col.rgb;\n"
    "    return ToRGB(col);\n"
    "}\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    float4 gi = giTex.SampleLevel(samp, uv, 0);\n"
    "    float3 albedo = saturate(DecodeAlbedo(int2(pos.xy)));\n"
    "    return float4(gi.rgb * albedo * p.x, 1.0);\n"
    "}\n";

static const char* kDebugPsSrc =
    "Texture2D giTex : register(t0);\n"
    "Texture2D clayTex : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer P : register(b0) { float4 p; }   // y = mode\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    int mode = (int)p.y;\n"
    "    if (mode == 1) return float4(clayTex.SampleLevel(samp, uv, 0).rgb, 1);\n"
    "    float4 gi = giTex.SampleLevel(samp, uv, 0);\n"
    "    if (mode == 2) return float4(gi.aaa, 1);\n"
    "    return float4(gi.rgb, 1);\n"
    "}\n";

static bool CompilePS(ID3D11Device* dev, const char* src, const char* name, ID3D11PixelShader** out)
{
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr) || !blob)
    {
        if (err) { Log("RtSystem: %s compile: %s", name, (const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    if (err) err->Release();
    hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
    blob->Release();
    return SUCCEEDED(hr);
}

static bool InitComposite(ID3D11Device* dev)
{
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kFsVsSrc, strlen(kFsVsSrc), "RtFsVS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &err);
    if (err) err->Release();
    if (FAILED(hr) || !blob) return false;
    hr = dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sFsVS);
    blob->Release();
    if (FAILED(hr)) return false;

    if (!CompilePS(dev, kCompositePsSrc, "RtCompositePS", &sCompositePS)) return false;
    if (!CompilePS(dev, kDebugPsSrc, "RtDebugPS", &sDebugPS)) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &sAddBlend))) return false;
    bd.RenderTarget[0].BlendEnable = FALSE;
    if (FAILED(dev->CreateBlendState(&bd, &sOpaqueBlend))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dd, &sNoDepth))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &sNoCull))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &sLinearSamp))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &sCompositeCB))) return false;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &sRtParamsCB))) return false;
    return true;
}

static DXGI_FORMAT SrvFormatFor(DXGI_FORMAT f)
{
    switch (f)
    {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_R32_FLOAT;
        default: return f;
    }
}

static ID3D11Texture2D* TexFromSRV(ID3D11ShaderResourceView* srv)
{
    if (!srv) return nullptr;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(IID_PPV_ARGS(&tex));
    res->Release();
    return tex;   // caller releases
}

static void ReleaseTargets()
{
    if (sGiOutSRV11) { sGiOutSRV11->Release(); sGiOutSRV11 = nullptr; }
    if (sDebugSRV11) { sDebugSRV11->Release(); sDebugSRV11 = nullptr; }
    sDepthSh.Release();
    sNormalsSh.Release();
    sGiOutSh.Release();
    sDebugSh.Release();
    if (sGiRaw) { sGiRaw->Release(); sGiRaw = nullptr; }
    for (int i = 0; i < 2; ++i)
    {
        if (sAccum[i]) { sAccum[i]->Release(); sAccum[i] = nullptr; }
        if (sAtrousTmp[i]) { sAtrousTmp[i]->Release(); sAtrousTmp[i] = nullptr; }
    }
    if (sPrevDepth) { sPrevDepth->Release(); sPrevDepth = nullptr; }
    if (sDebugDepth) { sDebugDepth->Release(); sDebugDepth = nullptr; }
    if (sDebugNorm) { sDebugNorm->Release(); sDebugNorm = nullptr; }
    sWidth = sHeight = 0;
    sHistoryValid = false;
}

static bool EnsureTargets(ID3D11Device* dev, ID3D11ShaderResourceView* depthSRV,
                          ID3D11ShaderResourceView* normalsSRV, uint32_t w, uint32_t h)
{
    if (sWidth == w && sHeight == h && sDepthSh.tex11)
        return true;

    // resolution change: GPU must be idle on our resources before re-creating
    RtCore::CpuWait(sTraceFence);
    ReleaseTargets();

    ID3D11Texture2D* depthTex = TexFromSRV(depthSRV);
    ID3D11Texture2D* normTex = TexFromSRV(normalsSRV);
    if (!depthTex || !normTex)
    {
        if (depthTex) depthTex->Release();
        if (normTex) normTex->Release();
        return false;
    }
    D3D11_TEXTURE2D_DESC dd = {}, nd = {};
    depthTex->GetDesc(&dd);
    normTex->GetDesc(&nd);
    depthTex->Release();
    normTex->Release();
    if (dd.Width != w || dd.Height != h || dd.SampleDesc.Count != 1)
        return false;
    sDepthSrcFmt = dd.Format;
    sNormalsSrcFmt = nd.Format;

    bool ok = true;
    ok &= RtCore::CreateSharedTexture(w, h, dd.Format, D3D11_BIND_SHADER_RESOURCE, sDepthSh);
    ok &= RtCore::CreateSharedTexture(w, h, nd.Format, D3D11_BIND_SHADER_RESOURCE, sNormalsSh);
    ok &= RtCore::CreateSharedTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, sGiOutSh);
    ok &= RtCore::CreateSharedTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, sDebugSh);

    sGiRaw = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    sAccum[0] = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    sAccum[1] = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    sAtrousTmp[0] = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    sAtrousTmp[1] = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    sPrevDepth = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R32_FLOAT);
    sDebugDepth = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R32_FLOAT);
    sDebugNorm = RtCore::CreateUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok &= sGiRaw && sAccum[0] && sAccum[1] && sAtrousTmp[0] && sAtrousTmp[1] &&
          sPrevDepth && sDebugDepth && sDebugNorm;

    if (ok)
    {
        ok &= SUCCEEDED(dev->CreateShaderResourceView(sGiOutSh.tex11, nullptr, &sGiOutSRV11));
        ok &= SUCCEEDED(dev->CreateShaderResourceView(sDebugSh.tex11, nullptr, &sDebugSRV11));
    }

    if (!ok)
    {
        ReleaseTargets();
        SetStatus("target creation failed");
        return false;
    }
    sWidth = w; sHeight = h;
    sHistoryValid = false;
    Log("RtSystem: targets %ux%u (depth fmt %d, normals fmt %d)", w, h, (int)sDepthSrcFmt, (int)sNormalsSrcFmt);
    return true;
}

// ---------------------------------------------------------------------------
// descriptors
// ---------------------------------------------------------------------------
static void SrvTex(RtCore::Descs& t, uint32_t i, ID3D12Resource* res, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    RtCore::Device()->CreateShaderResourceView(res, &d, t.Cpu(i));
}

static void SrvNull(RtCore::Descs& t, uint32_t i)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    RtCore::Device()->CreateShaderResourceView(nullptr, &d, t.Cpu(i));
}

static void SrvTlas(RtCore::Descs& t, uint32_t i, ID3D12Resource* tlas)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.RaytracingAccelerationStructure.Location = tlas->GetGPUVirtualAddress();
    RtCore::Device()->CreateShaderResourceView(nullptr, &d, t.Cpu(i));
}

static void SrvStructured(RtCore::Descs& t, uint32_t i, ID3D12Resource* res,
                          uint32_t stride, uint32_t elements)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = elements;
    d.Buffer.StructureByteStride = stride;
    RtCore::Device()->CreateShaderResourceView(res, &d, t.Cpu(i));
}

static void UavTex(RtCore::Descs& t, uint32_t i, ID3D12Resource* res, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    RtCore::Device()->CreateUnorderedAccessView(res, nullptr, &d, t.Cpu(i));
}

static void UavNull(RtCore::Descs& t, uint32_t i)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    RtCore::Device()->CreateUnorderedAccessView(nullptr, nullptr, &d, t.Cpu(i));
}

static void UavRawBuffer(RtCore::Descs& t, uint32_t i, ID3D12Resource* res, uint32_t numU32s)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = numU32s;
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    RtCore::Device()->CreateUnorderedAccessView(res, nullptr, &d, t.Cpu(i));
}

static void UavBarrier(ID3D12GraphicsCommandList4* cmd)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &b);
}

static void Transition(ID3D12GraphicsCommandList4* cmd, ID3D12Resource* res,
                       D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------
// Row-major 4x4 multiply: out = a x b
static void MatMulRow(float* out, const float* a, const float* b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] +
                         a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
}

// Row-major 4x4 inverse (same routine as GeometryReplay's, kept local).
static bool Inv4x4(float* out, const float* m)
{
    double a[16]; for (int i = 0; i < 16; ++i) a[i] = (double)m[i];
    double inv[16];
    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (det > -1e-12 && det < 1e-12) return false;
    double idet = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] * idet);
    return true;
}

// Camera far-clip from the projection matrix (column-major memory, used as
// mul(proj, columnVec)): unproject the NDC depth extremes and take the farther
// view-space distance (robust to reversed-Z). The GBuffer's normalized ray
// distance is depth01 * farClip — see gbuffer.hlsl / deferred.hlsl.
static float FarClipFromProj(const float* cm)
{
    float M[16];   // row-major view of the column-vector matrix
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            M[r * 4 + c] = cm[c * 4 + r];
    float inv[16];
    if (!Inv4x4(inv, M))
        return 0.0f;
    float best = 0.0f;
    const float zs[2] = { 1.0f, 0.0f };
    for (int i = 0; i < 2; ++i)
    {
        const float ndc[4] = { 0.0f, 0.0f, zs[i], 1.0f };
        float v[4];
        for (int r = 0; r < 4; ++r)
            v[r] = inv[r*4+0]*ndc[0] + inv[r*4+1]*ndc[1] + inv[r*4+2]*ndc[2] + inv[r*4+3]*ndc[3];
        if (fabsf(v[3]) < 1e-9f) continue;
        float d = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) / fabsf(v[3]);
        if (d == d && d > best && d < 1.0e7f)
            best = d;
    }
    return best;
}

static bool FillConstants(RtConstants& c)
{
    float iv[16], proj[16];
    if (!D3D11Hook::GetCameraMatrices(iv, proj))
        return false;
    if (fabsf(proj[0]) < 1e-6f || fabsf(proj[5]) < 1e-6f)
        return false;
    memcpy(sLastIv, iv, 64);
    memcpy(sLastProj, proj, 64);

    const float tanHalfFov = 1.0f / proj[5];
    const float aspect = proj[5] / proj[0];

    // Camera basis + origin: derived from the SAME view-proj the captured
    // GBuffer draws used (GeometryReplay's coherent camVP) — V = camVP·P⁻¹,
    // then invert V. This is the VB-space camera by construction; the deferred
    // inverseView (gCameraData) lives in a different (rebased) frame and put
    // every ray kilometers off (measured 2026-06-11: TLAS town ~1.4 km above
    // the deferred camera's scene reconstruction).
    bool camDerived = false;
    {
        float camVP[16];
        if (GeometryReplay::GetCameraVP(camVP))
        {
            float pInv[16], V[16], ivVb[16];
            if (Inv4x4(pInv, proj))
            {
                MatMulRow(V, camVP, pInv);
                if (Inv4x4(ivVb, V))
                {
                    c.camRight[0] = ivVb[0];  c.camRight[1] = ivVb[1];  c.camRight[2] = ivVb[2];
                    c.camUp[0] = ivVb[4];     c.camUp[1] = ivVb[5];     c.camUp[2] = ivVb[6];
                    c.camForward[0] = ivVb[8]; c.camForward[1] = ivVb[9]; c.camForward[2] = ivVb[10];
                    c.camPos[0] = ivVb[12];   c.camPos[1] = ivVb[13];   c.camPos[2] = ivVb[14];
                    camDerived = true;
                    static bool sDerivedLogged = false;
                    if (!sDerivedLogged)
                    {
                        Log("RtSystem: camVP-derived camera pos=(%.0f, %.0f, %.0f) vs iv=(%.0f, %.0f, %.0f)",
                            ivVb[12], ivVb[13], ivVb[14], iv[12], iv[13], iv[14]);
                        sDerivedLogged = true;
                    }
                }
            }
        }
    }
    if (!camDerived)
    {
        c.camRight[0] = iv[0];  c.camRight[1] = iv[1];  c.camRight[2] = iv[2];
        c.camUp[0] = iv[4];     c.camUp[1] = iv[5];     c.camUp[2] = iv[6];
        c.camForward[0] = iv[8]; c.camForward[1] = iv[9]; c.camForward[2] = iv[10];
        c.camPos[0] = iv[12]; c.camPos[1] = iv[13]; c.camPos[2] = iv[14];
    }
    c.camRight[3] = tanHalfFov;
    c.camUp[3] = aspect;
    c.camForward[3] = (float)(sFrameCounter % 4096);
    c.camPos[3] = sSettings.maxRayDist;

    if (sPrevCamCaptured)
    {
        memcpy(c.prevCamRight, sPrevFrameCam.camRight, 16);
        memcpy(c.prevCamUp, sPrevFrameCam.camUp, 16);
        memcpy(c.prevCamForward, sPrevFrameCam.camForward, 16);
        memcpy(c.prevCamPos, sPrevFrameCam.camPos, 16);
    }
    c.prevCamForward[3] = sSettings.temporalAlpha;
    c.prevCamPos[3] = (sHistoryValid && sPrevCamCaptured) ? 1.0f : 0.0f;

    // Sun direction. Primary: the live directional Ogre::Light from SceneAccess
    // (works in RTW and CSM shadow modes; brightest directional wins — sun vs
    // moon). Secondary: the CSM cbuffer snapshot (CSM mode only). Both store
    // the direction light TRAVELS, so "toward the sun" is the negation (the
    // flip toggle covers a wrong guess).
    float sd[3] = { 0.35f, 0.80f, 0.45f };
    sSunSource = 0;
    {
        static SceneAccess::Light sLights[256];
        int n = SceneAccess::GetLights(sLights, 256);
        float bestIntensity = -1.0f;
        for (int i = 0; i < n; ++i)
        {
            if (sLights[i].type == SceneAccess::LIGHT_DIRECTIONAL &&
                sLights[i].intensity > bestIntensity)
            {
                bestIntensity = sLights[i].intensity;
                sd[0] = sLights[i].dir[0];
                sd[1] = sLights[i].dir[1];
                sd[2] = sLights[i].dir[2];
                sSunSource = 2;
            }
        }
    }
    if (sSunSource == 0)
    {
        DustCSMData csm = {};
        if (CSMCapture::GetSnapshot(&csm) != 0 && csm.valid)
        {
            sd[0] = csm.sunDirection[0];
            sd[1] = csm.sunDirection[1];
            sd[2] = csm.sunDirection[2];
            sSunSource = 1;
        }
    }
    if (sSunSource != 0)
    {
        const float sign = sSettings.flipSunDir ? 1.0f : -1.0f;
        sd[0] *= sign; sd[1] *= sign; sd[2] *= sign;
    }
    sSunValid = sSunSource != 0;
    float len = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
    if (len < 1e-5f) { sd[0] = 0; sd[1] = 1; sd[2] = 0; len = 1; }
    c.sunDir[0] = sd[0] / len; c.sunDir[1] = sd[1] / len; c.sunDir[2] = sd[2] / len;
    c.sunDir[3] = tanf(sSettings.sunAngularRadiusDeg * 3.14159265f / 180.0f);

    for (int i = 0; i < 3; ++i)
    {
        c.sunColor[i] = sSettings.sunColor[i] * sSettings.sunIntensity;
        c.skyHorizon[i] = sSettings.skyHorizon[i] * sSettings.skyIntensity;
        c.skyZenith[i] = sSettings.skyZenith[i] * sSettings.skyIntensity;
    }
    c.sunColor[3] = (float)sSettings.shadowRaysPerPixel;
    c.skyHorizon[3] = (float)sSettings.giRaysPerPixel;
    c.skyZenith[3] = (float)sSettings.giBounces;

    c.giParams[0] = sSettings.bounceAlbedo;
    c.giParams[1] = sSettings.normalBias;
    c.giParams[2] = sSettings.gi ? 1.0f : 0.0f;
    c.giParams[3] = sSettings.sunShadows ? 1.0f : 0.0f;

    // far clip: the GBuffer depth is (Euclidean ray distance / farClip) where
    // farClip is the GAME'S UNIFORM — read from the terrain PS $Globals (the
    // projection-derived value measured wrong: 119837 vs the real uniform).
    float farClip = 0.0f;
    bool farFromGame = TerrainTess::GetFarClip(&farClip);
    if (!farFromGame)
    {
        farClip = FarClipFromProj(proj);
        if (!(farClip > 50.0f && farClip < 1.0e6f))
            farClip = 4000.0f;
    }
    static bool sCamLogged = false;
    if (!sCamLogged && farFromGame)
    {
        Log("RtSystem: farClip=%.1f (game uniform), cam=(%.0f, %.0f, %.0f), ivCam=(%.0f, %.0f, %.0f)",
            farClip, c.camPos[0], c.camPos[1], c.camPos[2], iv[12], iv[13], iv[14]);
        sCamLogged = true;
    }
    c.misc[0] = farClip;
    c.misc[1] = 1.0f;   // atrous step, overwritten per pass
    c.misc[2] = sSettings.sigmaDepth;
    c.misc[3] = sSettings.sigmaNormal;

    c.res[0] = (float)sWidth;
    c.res[1] = (float)sHeight;
    c.res[2] = 1.0f / (float)sWidth;
    c.res[3] = 1.0f / (float)sHeight;
    return true;
}

// ---------------------------------------------------------------------------
// offline-replay snapshot
// ---------------------------------------------------------------------------
static void WriteTexDump(ID3D11DeviceContext* ctx, ID3D11Device* dev,
                         ID3D11Texture2D* tex, const char* path)
{
    if (!tex) return;
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging)))
        return;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))   // one-time stall, fine
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") == 0)
        {
            const uint32_t rowBytes = td.Width * 4;   // both dump formats are 4 bytes/px
            for (uint32_t y = 0; y < td.Height; ++y)
                fwrite((const uint8_t*)m.pData + (size_t)y * m.RowPitch, 1, rowBytes, f);
            fclose(f);
        }
        ctx->Unmap(staging, 0);
    }
    staging->Release();
}

static void DoSnapshotDump(ID3D11DeviceContext* ctx, ID3D11Device* dev, const RtConstants& cb)
{
    std::string dir = DustLogDir() + "rt_dump";
    CreateDirectoryA(dir.c_str(), nullptr);

    RtCore::CpuWait(sTraceFence);   // settle the in-flight trace

    WriteTexDump(ctx, dev, sDepthSh.tex11, (dir + "\\depth.bin").c_str());
    WriteTexDump(ctx, dev, sNormalsSh.tex11, (dir + "\\normals.bin").c_str());
    RtScene::DumpGeometry(dir.c_str());

    float tcam[3] = {};
    bool tcamOk = TerrainTess::GetCameraPos(tcam);
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "\\meta.txt").c_str(), "w") == 0)
    {
        fprintf(f, "width %u\nheight %u\n", sWidth, sHeight);
        fprintf(f, "farClip %.6f\n", cb.misc[0]);
        fprintf(f, "maxRayDist %.6f\n", cb.camPos[3]);
        fprintf(f, "tanHalfFov %.8f\naspect %.8f\n", cb.camRight[3], cb.camUp[3]);
        fprintf(f, "camPos %.4f %.4f %.4f\n", cb.camPos[0], cb.camPos[1], cb.camPos[2]);
        fprintf(f, "camRight %.6f %.6f %.6f\n", cb.camRight[0], cb.camRight[1], cb.camRight[2]);
        fprintf(f, "camUp %.6f %.6f %.6f\n", cb.camUp[0], cb.camUp[1], cb.camUp[2]);
        fprintf(f, "camForward %.6f %.6f %.6f\n", cb.camForward[0], cb.camForward[1], cb.camForward[2]);
        fprintf(f, "sunDir %.6f %.6f %.6f\n", cb.sunDir[0], cb.sunDir[1], cb.sunDir[2]);
        fprintf(f, "terrainCam %.4f %.4f %.4f %d\n", tcam[0], tcam[1], tcam[2], tcamOk ? 1 : 0);
        fprintf(f, "iv");
        for (int i = 0; i < 16; ++i) fprintf(f, " %.6f", sLastIv[i]);
        fprintf(f, "\nproj");
        for (int i = 0; i < 16; ++i) fprintf(f, " %.6f", sLastProj[i]);
        fprintf(f, "\n");
        float camVP[16];
        if (GeometryReplay::GetCameraVP(camVP))
        {
            fprintf(f, "camVP");
            for (int i = 0; i < 16; ++i) fprintf(f, " %.6f", camVP[i]);
            fprintf(f, "\n");
        }
        fclose(f);
    }

    // raw clip matrices for offline decomposition checks
    {
        float mats[16 * 16];
        int place[16];
        uint32_t n = GeometryReplay::CopyDebugClips(mats, place, 16);
        FILE* fc = nullptr;
        if (n && fopen_s(&fc, (dir + "\\clips.txt").c_str(), "w") == 0)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                fprintf(fc, "clip %u place=%d", i, place[i]);
                for (int k = 0; k < 16; ++k) fprintf(fc, " %.6f", mats[i * 16 + k]);
                fprintf(fc, "\n");
            }
            fclose(fc);
        }
    }
    Log("RtSystem: RT snapshot dumped to %s", dir.c_str());
}

// ---------------------------------------------------------------------------
// frame: before the deferred lighting draw (GBuffer complete)
// ---------------------------------------------------------------------------
void OnLightingPre(ID3D11DeviceContext* ctx, ID3D11Device* device,
                   ID3D11ShaderResourceView* depthSRV, ID3D11ShaderResourceView* normalsSRV,
                   uint32_t width, uint32_t height, uint64_t frameIndex)
{
    sFrameTraced = false;
    if (!sSettings.enabled || sInit == InitState::Failed)
        return;
    if (!ctx || !device || !depthSRV || !normalsSRV || width < 2 || height < 2)
        return;

    if (sInit == InitState::Uninit)
    {
        // one-shot bring-up; any failure disables the system for the session
        sInit = InitState::Failed;   // assume failure until everything passes
        if (!RtCore::Init(device)) { SetStatus(RtCore::StatusString()); return; }
        if (!CompileShaders())      { SetStatus("shader compilation failed (see log)"); return; }
        if (!InitComposite(device)) { SetStatus("composite shader init failed"); return; }
        RtScene::Init();
        sInit = InitState::Ready;
        SetStatus("active (DXR tier 1.1)");
    }

    if (sFlushRequested)
    {
        sFlushRequested = false;
        RtScene::Flush();
        sHistoryValid = false;
    }

    if (!EnsureTargets(device, depthSRV, normalsSRV, width, height))
        return;

    sFrameCounter = frameIndex;

    RtConstants cb = {};
    if (!FillConstants(cb))
        return;   // camera not extracted yet (first frames)

    // 1) copy the GBuffer inputs into the shared textures (same format+size)
    ID3D11Texture2D* depthTex = TexFromSRV(depthSRV);
    ID3D11Texture2D* normTex = TexFromSRV(normalsSRV);
    if (!depthTex || !normTex)
    {
        if (depthTex) depthTex->Release();
        if (normTex) normTex->Release();
        return;
    }
    ctx->CopyResource(sDepthSh.tex11, depthTex);
    ctx->CopyResource(sNormalsSh.tex11, normTex);
    depthTex->Release();
    normTex->Release();

    // 2) 11 -> 12 dependency
    uint64_t vCopy = RtCore::NextFenceValue();
    if (!RtCore::Signal11(ctx, vCopy))
        return;
    RtCore::Wait12(vCopy);

    // 3) record + submit the D3D12 frame
    ID3D12GraphicsCommandList4* cmd = RtCore::Begin();
    if (!cmd) return;

    const uint32_t frameSlot = (uint32_t)(frameIndex % 3);
    bool hasTlas = RtScene::RecordBuild(cmd, frameSlot);

    const uint32_t gx = (sWidth + 7) / 8, gy = (sHeight + 7) / 8;
    const DXGI_FORMAT depthViewFmt = SrvFormatFor(sDepthSrcFmt);
    const DXGI_FORMAT normViewFmt = SrvFormatFor(sNormalsSrcFmt);

    if (hasTlas)
    {
        cmd->SetComputeRootSignature(sRootSig);

        // ---- trace ------------------------------------------------------
        {
            D3D12_GPU_VIRTUAL_ADDRESS cbVA = RtCore::AllocCB(&cb, sizeof(cb));
            RtCore::Descs srvs = RtCore::AllocDescs(6);
            RtCore::Descs uavs = RtCore::AllocDescs(4);
            SrvTex(srvs, 0, sDepthSh.tex12, depthViewFmt);
            SrvTex(srvs, 1, sNormalsSh.tex12, normViewFmt);
            SrvTlas(srvs, 2, RtScene::Tlas());
            SrvStructured(srvs, 3, RtScene::IdxPool(), 4, 15 * 1024 * 1024);
            SrvStructured(srvs, 4, RtScene::PosPool(), 12, 5 * 1024 * 1024);
            SrvStructured(srvs, 5, RtScene::InstInfoBuffer(frameSlot), 16, 8192);
            UavTex(uavs, 0, sGiRaw, DXGI_FORMAT_R16G16B16A16_FLOAT);
            UavNull(uavs, 1); UavNull(uavs, 2); UavNull(uavs, 3);

            cmd->SetPipelineState(sPsoTrace);
            cmd->SetComputeRootConstantBufferView(0, cbVA);
            cmd->SetComputeRootDescriptorTable(1, srvs.gpu);
            cmd->SetComputeRootDescriptorTable(2, uavs.gpu);
            cmd->Dispatch(gx, gy, 1);
            UavBarrier(cmd);
        }

        // ---- temporal accumulation --------------------------------------
        ID3D12Resource* history = sAccum[sAccumPing];
        ID3D12Resource* accumOut = sAccum[sAccumPing ^ 1];
        {
            D3D12_GPU_VIRTUAL_ADDRESS cbVA = RtCore::AllocCB(&cb, sizeof(cb));
            RtCore::Descs srvs = RtCore::AllocDescs(6);
            RtCore::Descs uavs = RtCore::AllocDescs(4);
            SrvTex(srvs, 0, sDepthSh.tex12, depthViewFmt);
            SrvTex(srvs, 1, sNormalsSh.tex12, normViewFmt);
            for (uint32_t i = 2; i < 6; ++i) SrvNull(srvs, i);
            UavTex(uavs, 0, sGiRaw, DXGI_FORMAT_R16G16B16A16_FLOAT);
            UavTex(uavs, 1, history, DXGI_FORMAT_R16G16B16A16_FLOAT);
            UavTex(uavs, 2, accumOut, DXGI_FORMAT_R16G16B16A16_FLOAT);
            UavTex(uavs, 3, sPrevDepth, DXGI_FORMAT_R32_FLOAT);

            cmd->SetPipelineState(sPsoTemporal);
            cmd->SetComputeRootConstantBufferView(0, cbVA);
            cmd->SetComputeRootDescriptorTable(1, srvs.gpu);
            cmd->SetComputeRootDescriptorTable(2, uavs.gpu);
            cmd->Dispatch(gx, gy, 1);
            UavBarrier(cmd);
        }

        // current depth becomes next frame's prev-depth
        cmd->CopyResource(sPrevDepth, sDepthSh.tex12);

        // ---- a-trous chain, final pass lands in the shared output --------
        Transition(cmd, sGiOutSh.tex12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        {
            int iters = sSettings.atrousIterations;
            if (iters < 1) iters = 1;
            if (iters > 4) iters = 4;
            ID3D12Resource* src = accumOut;
            for (int k = 0; k < iters; ++k)
            {
                ID3D12Resource* dst = (k == iters - 1) ? sGiOutSh.tex12 : sAtrousTmp[k & 1];
                RtConstants cbp = cb;
                cbp.misc[1] = (float)(1 << k);
                D3D12_GPU_VIRTUAL_ADDRESS cbVA = RtCore::AllocCB(&cbp, sizeof(cbp));
                RtCore::Descs srvs = RtCore::AllocDescs(6);
                RtCore::Descs uavs = RtCore::AllocDescs(4);
                SrvTex(srvs, 0, sDepthSh.tex12, depthViewFmt);
                SrvTex(srvs, 1, sNormalsSh.tex12, normViewFmt);
                for (uint32_t i = 2; i < 6; ++i) SrvNull(srvs, i);
                UavTex(uavs, 0, src, DXGI_FORMAT_R16G16B16A16_FLOAT);
                UavTex(uavs, 1, dst, DXGI_FORMAT_R16G16B16A16_FLOAT);
                UavNull(uavs, 2); UavNull(uavs, 3);

                cmd->SetPipelineState(sPsoAtrous);
                cmd->SetComputeRootConstantBufferView(0, cbVA);
                cmd->SetComputeRootDescriptorTable(1, srvs.gpu);
                cmd->SetComputeRootDescriptorTable(2, uavs.gpu);
                cmd->Dispatch(gx, gy, 1);
                UavBarrier(cmd);
                src = dst;
            }
        }
        Transition(cmd, sGiOutSh.tex12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

        // ---- debug clay view + TLAS/GBuffer alignment measurement -----------
        if (sSettings.debugMode == 1)
        {
            // lazy alignment instrumentation buffers
            if (!sAlignBuf)
            {
                const uint32_t zeros[4] = {};
                sAlignBuf = RtCore::CreateBuffer(16, D3D12_HEAP_TYPE_DEFAULT,
                                                 D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                sAlignZeros = RtCore::CreateUploadBuffer(zeros, 16);
                for (int i = 0; i < 3; ++i)
                    sAlignReadback[i] = RtCore::CreateBuffer(16, D3D12_HEAP_TYPE_READBACK,
                                                             D3D12_RESOURCE_STATE_COPY_DEST);
            }

            Transition(cmd, sDebugSh.tex12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (sAlignBuf && sAlignZeros)
            {
                cmd->CopyBufferRegion(sAlignBuf, 0, sAlignZeros, 0, 16);
                UavBarrier(cmd);
            }

            D3D12_GPU_VIRTUAL_ADDRESS cbVA = RtCore::AllocCB(&cb, sizeof(cb));
            RtCore::Descs srvs = RtCore::AllocDescs(6);
            RtCore::Descs uavs = RtCore::AllocDescs(4);
            SrvTex(srvs, 0, sDepthSh.tex12, depthViewFmt);
            SrvNull(srvs, 1);
            SrvTlas(srvs, 2, RtScene::Tlas());
            SrvStructured(srvs, 3, RtScene::IdxPool(), 4, 15 * 1024 * 1024);
            SrvStructured(srvs, 4, RtScene::PosPool(), 12, 5 * 1024 * 1024);
            SrvStructured(srvs, 5, RtScene::InstInfoBuffer(frameSlot), 16, 8192);
            UavTex(uavs, 0, sDebugSh.tex12, DXGI_FORMAT_R8G8B8A8_UNORM);
            UavTex(uavs, 1, sDebugDepth, DXGI_FORMAT_R32_FLOAT);
            UavTex(uavs, 2, sDebugNorm, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (sAlignBuf) UavRawBuffer(uavs, 3, sAlignBuf, 4); else UavNull(uavs, 3);

            cmd->SetPipelineState(sPsoDebug);
            cmd->SetComputeRootConstantBufferView(0, cbVA);
            cmd->SetComputeRootDescriptorTable(1, srvs.gpu);
            cmd->SetComputeRootDescriptorTable(2, uavs.gpu);
            cmd->Dispatch(gx, gy, 1);
            Transition(cmd, sDebugSh.tex12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

            // counters -> readback ring; consumed CPU-side once the fence passes
            if (sAlignBuf)
            {
                UavBarrier(cmd);
                const uint32_t slot = (uint32_t)(frameIndex % 3);
                cmd->CopyBufferRegion(sAlignReadback[slot], 0, sAlignBuf, 0, 16);
            }
        }

        sAccumPing ^= 1;
        sHistoryValid = true;
        sFrameTraced = true;
    }

    uint64_t done = RtCore::Submit();
    if (done == 0)
    {
        sFrameTraced = false;
        return;
    }
    sTraceFence = done;

    // offline-replay snapshot: GUI-requested, or automatic once a real scene
    // is populated (the iteration loop then moves fully offline)
    if (sFrameTraced &&
        (sDumpRequested || (!sAutoDumpDone && RtScene::InstanceCount() >= 100)))
    {
        sDumpRequested = false;
        sAutoDumpDone = true;
        DoSnapshotDump(ctx, device, cb);
    }

    // alignment readback: stamp this frame's ring slot, consume the oldest one
    if (sSettings.debugMode == 1 && sAlignBuf)
    {
        const uint32_t slot = (uint32_t)(frameIndex % 3);
        sAlignFence[slot] = done;
        const uint32_t oldSlot = (uint32_t)((frameIndex + 1) % 3);
        if (sAlignFence[oldSlot] && RtCore::CompletedValue() >= sAlignFence[oldSlot])
        {
            uint32_t* counts = nullptr;
            D3D12_RANGE rr = { 0, 16 };
            if (SUCCEEDED(sAlignReadback[oldSlot]->Map(0, &rr, (void**)&counts)))
            {
                if (counts[0] > 1000)
                    sAlignPct = 100.0f * (float)counts[1] / (float)counts[0];
                sAlignReadback[oldSlot]->Unmap(0, nullptr);
            }
        }
    }
    else
        sAlignPct = -1.0f;

    // stash this frame's camera for next frame's reprojection
    memcpy(&sPrevFrameCam, &cb, sizeof(cb));
    sPrevCamCaptured = true;
}

// ---------------------------------------------------------------------------
// frame: after the lighting draw (before the additive light volumes)
// ---------------------------------------------------------------------------
static void DrawFullscreen(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                           ID3D11PixelShader* ps, ID3D11ShaderResourceView* t0,
                           ID3D11ShaderResourceView* t1, ID3D11BlendState* blend,
                           float intensity, float mode)
{
    D3D11StateBlock saved;
    saved.Capture(ctx);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(sCompositeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        float p[4] = { intensity, mode, 0, 0 };
        memcpy(m.pData, p, 16);
        ctx->Unmap(sCompositeCB, 0);
    }

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(sFsVS, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { t0, t1 };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &sLinearSamp);
    ctx->PSSetConstantBuffers(0, 1, &sCompositeCB);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float bf[4] = { 1, 1, 1, 1 };
    ctx->OMSetBlendState(blend, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(sNoDepth, 0);
    ctx->RSSetState(sNoCull);
    D3D11_VIEWPORT vp = { 0, 0, (float)sWidth, (float)sHeight, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);

    // unbind our SRVs before restore so the GI texture isn't left bound
    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nulls);
    saved.Restore(ctx);
}

void BindForLightingDraw(ID3D11DeviceContext* ctx)
{
    if (sInit != InitState::Ready || !ctx || !sRtParamsCB)
        return;
    if (!ShaderPatch::IsRtMainFsPatched())
        return;

    const bool live = sSettings.enabled && sFrameTraced && sGiOutSRV11 != nullptr;
    float p[4] = { 0, 0, 0, 0 };   // zeros = vanilla lighting in the patch
    if (live)
    {
        // GPU-side wait so the lighting draw reads THIS frame's trace
        RtCore::Wait11(ctx, sTraceFence);
        p[0] = sSettings.gi ? sSettings.ambientReplace : 0.0f;
        p[1] = sSettings.gi ? sSettings.giIntensity : 0.0f;
        p[2] = sSettings.sunShadows ? sSettings.sunShadowStrength : 0.0f;
        p[3] = sSettings.sunShadows ? sSettings.vanillaShadowRemove : 0.0f;
    }

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(sRtParamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, p, 16);
        ctx->Unmap(sRtParamsCB, 0);
    }
    ctx->PSSetConstantBuffers(9, 1, &sRtParamsCB);
    ID3D11ShaderResourceView* srv = live ? sGiOutSRV11 : nullptr;
    ctx->PSSetShaderResources(20, 1, &srv);
}

void OnLightingPost(ID3D11DeviceContext* ctx, ID3D11Device* device,
                    ID3D11ShaderResourceView* albedoSRV,
                    uint32_t width, uint32_t height)
{
    if (sInit != InitState::Ready || !sSettings.enabled || !ctx || !device)
        return;
    if (width != sWidth || height != sHeight)
        return;

    const bool patched = ShaderPatch::IsRtMainFsPatched();
    if (sFrameTraced)
    {
        // patched path already queued its wait before the lighting draw
        if (!patched)
            RtCore::Wait11(ctx, sTraceFence);

        // the HDR scene target is what the lighting draw just wrote
        ID3D11RenderTargetView* hdrRTV = nullptr;
        ctx->OMGetRenderTargets(1, &hdrRTV, nullptr);

        // additive composite is the FALLBACK for unpatched main_fs only —
        // when patched, the GI/ambient mix happens inside the lighting shader
        if (!patched && sSettings.gi && sSettings.debugMode == 0 && hdrRTV && albedoSRV && sGiOutSRV11)
            DrawFullscreen(ctx, hdrRTV, sCompositePS, sGiOutSRV11, albedoSRV,
                           sAddBlend, sSettings.giIntensity, 0.0f);

        if (sSettings.debugMode != 0 && hdrRTV && sGiOutSRV11 && sDebugSRV11)
            DrawFullscreen(ctx, hdrRTV, sDebugPS, sGiOutSRV11, sDebugSRV11,
                           sOpaqueBlend, 1.0f, (float)sSettings.debugMode);

        if (hdrRTV) hdrRTV->Release();
    }

    // schedule geometry work for the NEXT frame (capture pointers valid here)
    RtScene::Harvest(ctx, device);
}

void Shutdown()
{
    if (sInit == InitState::Uninit)
        return;
    RtScene::Shutdown();
    ReleaseTargets();
    for (int i = 0; i < 4; ++i)
        if (sShaderBlobs[i]) { sShaderBlobs[i]->Release(); sShaderBlobs[i] = nullptr; }
    if (sPsoTrace)    { sPsoTrace->Release();    sPsoTrace = nullptr; }
    if (sPsoTemporal) { sPsoTemporal->Release(); sPsoTemporal = nullptr; }
    if (sPsoAtrous)   { sPsoAtrous->Release();   sPsoAtrous = nullptr; }
    if (sPsoDebug)    { sPsoDebug->Release();    sPsoDebug = nullptr; }
    if (sRootSig)     { sRootSig->Release();     sRootSig = nullptr; }
    if (sFsVS)        { sFsVS->Release();        sFsVS = nullptr; }
    if (sCompositePS) { sCompositePS->Release(); sCompositePS = nullptr; }
    if (sDebugPS)     { sDebugPS->Release();     sDebugPS = nullptr; }
    if (sAddBlend)    { sAddBlend->Release();    sAddBlend = nullptr; }
    if (sOpaqueBlend) { sOpaqueBlend->Release(); sOpaqueBlend = nullptr; }
    if (sNoDepth)     { sNoDepth->Release();     sNoDepth = nullptr; }
    if (sNoCull)      { sNoCull->Release();      sNoCull = nullptr; }
    if (sLinearSamp)  { sLinearSamp->Release();  sLinearSamp = nullptr; }
    if (sCompositeCB) { sCompositeCB->Release(); sCompositeCB = nullptr; }
    if (sRtParamsCB)  { sRtParamsCB->Release();  sRtParamsCB = nullptr; }
    if (sAlignBuf)    { sAlignBuf->Release();    sAlignBuf = nullptr; }
    if (sAlignZeros)  { sAlignZeros->Release();  sAlignZeros = nullptr; }
    for (int i = 0; i < 3; ++i)
        if (sAlignReadback[i]) { sAlignReadback[i]->Release(); sAlignReadback[i] = nullptr; }
    RtCore::Shutdown();
    sInit = InitState::Uninit;
}

} // namespace RtSystem
