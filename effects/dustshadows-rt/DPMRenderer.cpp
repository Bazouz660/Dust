#include "DPMRenderer.h"
#include "DustLog.h"
#include <cstring>
#include <cmath>
#include <string>

namespace DPMRenderer
{

static bool gInitialized = false;
static const DustHostAPI* gHost = nullptr;
static UINT gWidth = 0;
static UINT gHeight = 0;
static std::string gShaderDir;

static Config gConfig;

// DPM resources
static int                       gDpmAllocSize = 0;     // current allocated dimension
static ID3D11Texture2D*          gCountTex = nullptr;   // R32_UINT, NxN — triangle count per texel
static ID3D11UnorderedAccessView* gCountUAV = nullptr;
static ID3D11ShaderResourceView*  gCountSRV = nullptr;

// Depth target for the rasterized DPM build pass (also serves as conventional shadow map later)
static ID3D11Texture2D*          gDepthTex = nullptr;
static ID3D11DepthStencilView*   gDepthDSV = nullptr;
static ID3D11ShaderResourceView* gDepthSRV = nullptr;

// Shaders
static ID3D11GeometryShader* gBuildGS = nullptr;
static ID3D11PixelShader*    gBuildPS = nullptr;
static ID3D11PixelShader*    gDebugPS = nullptr;

// Pipeline state
static ID3D11RasterizerState*   gRSNoCull = nullptr;
static ID3D11DepthStencilState* gDSSDefault = nullptr;
static ID3D11BlendState*        gNoBlend = nullptr;
static ID3D11SamplerState*      gPointSampler = nullptr;

// Constant buffers
static ID3D11Buffer* gBuildCB = nullptr; // GS+PS shared CB: invLightVP + dpmSize
static ID3D11Buffer* gDebugCB = nullptr; // Debug PS CB: dpmSize, maxDisplay

// Sun direction extraction (mirrors SSSRenderer pattern)
static ID3D11Buffer* gSunStagingCB = nullptr;
static float gSunDir[3] = { 0.0f, 1.0f, 0.0f };
static bool  gHasSunDir = false;

// CB layouts
struct alignas(16) BuildCBData
{
    float invLightVP[16]; // row-major
    float dpmSize;
    float maxDepth;       // = gConfig.maxPrimsPerTexel, used by PS to cap slot writes
    float pad[2];
};

struct alignas(16) DebugCBData
{
    float viewportSize[2];
    float dpmSize;
    float maxDisplay;       // upper bound for heatmap normalization
};

// DPM Phase 2: prim buffer + indices map
static int                        gPrimAllocSize = 0;     // current prim buffer capacity
static ID3D11Buffer*              gIndicesBuf = nullptr;  // StructuredBuffer<uint>, N*N*d
static ID3D11UnorderedAccessView* gIndicesUAV = nullptr;
static ID3D11ShaderResourceView*  gIndicesSRV = nullptr;

static ID3D11Buffer*              gPrimBuf = nullptr;     // StructuredBuffer<DPMPrim>, maxPrims
static ID3D11UnorderedAccessView* gPrimUAV = nullptr;
static ID3D11ShaderResourceView*  gPrimSRV = nullptr;

static ID3D11Buffer*              gDrawOffsetCB = nullptr; // per-draw CB: uint drawCallOffset

// Shadow mask outputs (screen-resolution)
static UINT                       gShadowW = 0, gShadowH = 0;
static ID3D11Texture2D*           gShadowTex = nullptr;   // R8_UNORM
static ID3D11UnorderedAccessView* gShadowUAV = nullptr;
static ID3D11ShaderResourceView*  gShadowSRV = nullptr;
static bool                       gShadowMaskReady = false;

// Phase 2 shaders
static ID3D11ComputeShader*  gRayTraceCS = nullptr;
static ID3D11PixelShader*    gShadowDebugPS = nullptr;
static ID3D11Buffer*         gRayTraceCB = nullptr;
static ID3D11Buffer*         gShadowDebugCB = nullptr;

// Sun VP cached from last BuildDPM (needed by RayTrace to project screen→DPM)
static float gLastLightVP[16] = {};
static float gLastCamPos[3]   = {};

// ---------- shaders (compiled from disk at init) ----------

static const char* kBuildGS_File      = "dpm_build_gs.hlsl";
static const char* kBuildPS_File      = "dpm_build_ps.hlsl";
static const char* kDebugPS_File      = "dpm_debug_ps.hlsl";
static const char* kRayTraceCS_File   = "dpm_raytrace_cs.hlsl";
static const char* kShadowDebugPS_File = "dpm_shadow_debug_ps.hlsl";

// CB layouts
struct alignas(16) DrawOffsetCBData
{
    uint32_t drawCallOffset;
    uint32_t pad[3];
};

struct alignas(16) RayTraceCBData
{
    float inverseView[16]; // row-major inverse view matrix
    float lightVP[16];     // row-major light VP (world → DPM clip)
    float sunDir[3];
    float tanHalfFov;
    float viewportSize[2];
    float dpmSize;
    float maxDepth;
    uint32_t primBufSize;
    float pad[3];
};

// ---------- helpers ----------

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static void Vec3Sub(float* o, const float* a, const float* b)
{
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static void Vec3Cross(float* o, const float* a, const float* b)
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
static float Vec3Dot(const float* a, const float* b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static void Vec3Normalize(float* v)
{
    float len = sqrtf(Vec3Dot(v, v));
    if (len > 1e-6f) { v[0]/=len; v[1]/=len; v[2]/=len; }
}

// Build a sun-aligned ortho View*Projection matrix in row-major layout.
// View: orthonormal frame at cameraPos with -sun as forward axis.
// Proj: ortho [-extent, +extent] x, y; [-depth, +depth] z mapped to clip [-1,1].
// Returns the VP and its inverse (also row-major) for the GS to recover world-space.
static void ComputeSunVP(const float* sunDir, const float* cameraPos,
                         float extent, float depth,
                         float* outVP, float* outInvVP)
{
    // World-space frame: forward = -sun (toward shadowed scene)
    float fwd[3] = { -sunDir[0], -sunDir[1], -sunDir[2] };
    Vec3Normalize(fwd);

    float upHint[3] = { 0, 1, 0 };
    if (fabsf(fwd[1]) > 0.99f) { upHint[0] = 0; upHint[1] = 0; upHint[2] = 1; }

    float right[3];
    Vec3Cross(right, upHint, fwd);
    Vec3Normalize(right);

    float up[3];
    Vec3Cross(up, fwd, right);
    Vec3Normalize(up);

    // Light eye is at cameraPos. (Sun is directional; eye position only sets ortho center.)
    // View matrix (row-major, world -> light space). For row-major math with
    // row-vector multiplication: clipPos = pos * V * P.
    // Rows of V are (right, up, fwd) with the translation in column 3.
    float V[16] = {0};
    V[0] = right[0]; V[1] = up[0]; V[2] = fwd[0]; V[3] = 0;
    V[4] = right[1]; V[5] = up[1]; V[6] = fwd[1]; V[7] = 0;
    V[8] = right[2]; V[9] = up[2]; V[10] = fwd[2]; V[11] = 0;
    V[12] = -Vec3Dot(right, cameraPos);
    V[13] = -Vec3Dot(up,    cameraPos);
    V[14] = -Vec3Dot(fwd,   cameraPos);
    V[15] = 1;

    // Ortho projection (D3D-style, [0,1] z clip is also fine for shadow; use [-1,1] for symmetry)
    // x: [-extent, +extent] -> [-1, 1]
    // y: [-extent, +extent] -> [-1, 1]
    // z: [-depth,  +depth ] -> [ 0, 1]
    float P[16] = {0};
    P[0]  = 1.0f / extent;
    P[5]  = 1.0f / extent;
    P[10] = 0.5f / depth;
    P[14] = 0.5f;
    P[15] = 1;

    // VP = V * P (row-major)
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++) s += V[r*4+k] * P[k*4+c];
            outVP[r*4+c] = s;
        }

    // Compute the inverse VP. Since VP = V * P, invVP = invP * invV.
    float invP[16] = {0};
    invP[0]  = extent;
    invP[5]  = extent;
    invP[10] = 2.0f * depth;
    invP[14] = -depth;
    invP[15] = 1;

    // V is orthogonal (rotation+translation), so invV = transpose(rotation) and -translation
    float invV[16] = {0};
    invV[0] = V[0]; invV[1] = V[4]; invV[2] = V[8];  invV[3] = 0;
    invV[4] = V[1]; invV[5] = V[5]; invV[6] = V[9];  invV[7] = 0;
    invV[8] = V[2]; invV[9] = V[6]; invV[10]= V[10]; invV[11]= 0;
    invV[12] = cameraPos[0];
    invV[13] = cameraPos[1];
    invV[14] = cameraPos[2];
    invV[15] = 1;

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            float s = 0;
            for (int k = 0; k < 4; k++) s += invP[r*4+k] * invV[k*4+c];
            outInvVP[r*4+c] = s;
        }
}

static bool CompileShaders(ID3D11Device* device)
{
    auto compileFile = [&](const char* file, const char* entry, const char* target,
                           ID3DBlob** outBlob) -> bool
    {
        std::string path = gShaderDir + "\\" + file;
        ID3DBlob* blob = gHost->CompileShaderFromFile(path.c_str(), entry, target);
        if (!blob)
        {
            Log("DustShadowsRT: failed to compile %s", file);
            return false;
        }
        *outBlob = blob;
        return true;
    };

    ID3DBlob* gsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* dbgBlob = nullptr;
    bool ok = true;

    if (ok && compileFile(kBuildGS_File, "main", "gs_5_0", &gsBlob))
    {
        HRESULT hr = device->CreateGeometryShader(gsBlob->GetBufferPointer(),
                                                   gsBlob->GetBufferSize(),
                                                   nullptr, &gBuildGS);
        gsBlob->Release();
        if (FAILED(hr)) { Log("DustShadowsRT: CreateGeometryShader failed (0x%08X)", hr); ok = false; }
    } else ok = false;

    if (ok && compileFile(kBuildPS_File, "main", "ps_5_0", &psBlob))
    {
        HRESULT hr = device->CreatePixelShader(psBlob->GetBufferPointer(),
                                                psBlob->GetBufferSize(),
                                                nullptr, &gBuildPS);
        psBlob->Release();
        if (FAILED(hr)) { Log("DustShadowsRT: CreatePixelShader (build) failed (0x%08X)", hr); ok = false; }
    } else ok = false;

    if (ok && compileFile(kDebugPS_File, "main", "ps_5_0", &dbgBlob))
    {
        HRESULT hr = device->CreatePixelShader(dbgBlob->GetBufferPointer(),
                                                dbgBlob->GetBufferSize(),
                                                nullptr, &gDebugPS);
        dbgBlob->Release();
        if (FAILED(hr)) { Log("DustShadowsRT: CreatePixelShader (debug) failed (0x%08X)", hr); ok = false; }
    } else ok = false;

    // Phase 2 shaders — fail softly so Phase 1 still works if they're missing
    ID3DBlob* rtBlob  = nullptr;
    ID3DBlob* sdbBlob = nullptr;

    if (compileFile(kRayTraceCS_File, "main", "cs_5_0", &rtBlob))
    {
        HRESULT hr = device->CreateComputeShader(rtBlob->GetBufferPointer(),
                                                  rtBlob->GetBufferSize(),
                                                  nullptr, &gRayTraceCS);
        rtBlob->Release();
        if (FAILED(hr)) Log("DustShadowsRT: CreateComputeShader (raytrace) failed (0x%08X)", hr);
    }

    if (compileFile(kShadowDebugPS_File, "main", "ps_5_0", &sdbBlob))
    {
        HRESULT hr = device->CreatePixelShader(sdbBlob->GetBufferPointer(),
                                                sdbBlob->GetBufferSize(),
                                                nullptr, &gShadowDebugPS);
        sdbBlob->Release();
        if (FAILED(hr)) Log("DustShadowsRT: CreatePixelShader (shadow debug) failed (0x%08X)", hr);
    }

    return ok;
}

static bool CreateDPMResources(ID3D11Device* device, int size)
{
    SAFE_RELEASE(gCountTex); SAFE_RELEASE(gCountUAV); SAFE_RELEASE(gCountSRV);
    SAFE_RELEASE(gDepthTex); SAFE_RELEASE(gDepthDSV); SAFE_RELEASE(gDepthSRV);

    // Count map: R32_UINT, NxN, UAV+SRV
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = (UINT)size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &gCountTex)))
    { Log("DustShadowsRT: count map texture creation failed"); return false; }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateUnorderedAccessView(gCountTex, &uavDesc, &gCountUAV)))
    { Log("DustShadowsRT: count map UAV creation failed"); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(gCountTex, &srvDesc, &gCountSRV)))
    { Log("DustShadowsRT: count map SRV creation failed"); return false; }

    // Depth target: D32_FLOAT (typeless so we can SRV+DSV) — typeless layout
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = dd.Height = (UINT)size;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&dd, nullptr, &gDepthTex)))
    { Log("DustShadowsRT: depth tex creation failed"); return false; }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateDepthStencilView(gDepthTex, &dsvDesc, &gDepthDSV)))
    { Log("DustShadowsRT: DSV creation failed"); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC dsrvDesc = {};
    dsrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    dsrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    dsrvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(gDepthTex, &dsrvDesc, &gDepthSRV)))
    { Log("DustShadowsRT: depth SRV creation failed"); return false; }

    gDpmAllocSize = size;
    Log("DustShadowsRT: allocated DPM count+depth (%dx%d)", size, size);
    return true;
}

static bool CreatePhase2Resources(ID3D11Device* device, int dpmSize, int maxPrims)
{
    SAFE_RELEASE(gIndicesBuf); SAFE_RELEASE(gIndicesUAV); SAFE_RELEASE(gIndicesSRV);
    SAFE_RELEASE(gPrimBuf);    SAFE_RELEASE(gPrimUAV);    SAFE_RELEASE(gPrimSRV);
    SAFE_RELEASE(gDrawOffsetCB);

    uint32_t numIndices = (uint32_t)dpmSize * (uint32_t)dpmSize * (uint32_t)gConfig.maxPrimsPerTexel;

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

    // Indices map: N*N*d uints (prim indices per texel slot)
    bd.ByteWidth = numIndices * sizeof(uint32_t);
    bd.StructureByteStride = sizeof(uint32_t);
    if (FAILED(device->CreateBuffer(&bd, nullptr, &gIndicesBuf)))
    { Log("DustShadowsRT: indices buffer creation failed (%u MB)", bd.ByteWidth/(1024*1024)); return false; }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {};
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = numIndices;
    if (FAILED(device->CreateUnorderedAccessView(gIndicesBuf, &uavd, &gIndicesUAV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvd.Buffer.NumElements = numIndices;
    if (FAILED(device->CreateShaderResourceView(gIndicesBuf, &srvd, &gIndicesSRV))) return false;

    // Prim buffer: maxPrims × 36 bytes (float3 v0, float3 v1, float3 v2)
    bd.ByteWidth = (uint32_t)maxPrims * 9 * sizeof(float);
    bd.StructureByteStride = 9 * sizeof(float);
    if (FAILED(device->CreateBuffer(&bd, nullptr, &gPrimBuf)))
    { Log("DustShadowsRT: prim buffer creation failed (%u MB)", bd.ByteWidth/(1024*1024)); return false; }

    uavd.Buffer.NumElements = (uint32_t)maxPrims;
    if (FAILED(device->CreateUnorderedAccessView(gPrimBuf, &uavd, &gPrimUAV))) return false;
    srvd.Buffer.NumElements = (uint32_t)maxPrims;
    if (FAILED(device->CreateShaderResourceView(gPrimBuf, &srvd, &gPrimSRV))) return false;

    // Per-draw CB: drawCallOffset (updated by pre-draw callback)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &gDrawOffsetCB))) return false;

    gPrimAllocSize = maxPrims;
    Log("DustShadowsRT: Phase 2 DPM buffers: indices=%uMB prims=%uMB",
        numIndices * 4 / (1024*1024),
        maxPrims * 9 * 4 / (1024*1024));
    return true;
}

static bool CreateShadowMaskTextures(ID3D11Device* device, UINT w, UINT h)
{
    SAFE_RELEASE(gShadowTex); SAFE_RELEASE(gShadowUAV); SAFE_RELEASE(gShadowSRV);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &gShadowTex))) return false;
    if (FAILED(device->CreateUnorderedAccessView(gShadowTex, nullptr, &gShadowUAV))) return false;
    if (FAILED(device->CreateShaderResourceView(gShadowTex, nullptr, &gShadowSRV))) return false;
    gShadowW = w; gShadowH = h;
    return true;
}

static bool CreatePipelineState(ID3D11Device* device)
{
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE; // shadow caster geometry — no back-face cull
    rs.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rs, &gRSNoCull))) return false;

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&ds, &gDSSDefault))) return false;

    D3D11_BLEND_DESC bs = {};
    bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bs, &gNoBlend))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(device->CreateSamplerState(&sd, &gPointSampler))) return false;

    return true;
}

bool Init(ID3D11Device* device, UINT width, UINT height,
          const DustHostAPI* host, const char* effectDir)
{
    gHost = host;
    gWidth = width;
    gHeight = height;
    gShaderDir = std::string(effectDir) + "\\shaders";

    if (!CompileShaders(device))   return false;
    if (!CreatePipelineState(device)) return false;
    if (!CreateDPMResources(device, gConfig.dpmResolution)) return false;
    CreatePhase2Resources(device, gConfig.dpmResolution, gConfig.maxPrimBufferSize);
    CreateShadowMaskTextures(device, width, height);

    gBuildCB = host->CreateConstantBuffer(device, sizeof(BuildCBData));
    gDebugCB = host->CreateConstantBuffer(device, sizeof(DebugCBData));
    if (!gBuildCB || !gDebugCB) { Log("DustShadowsRT: CB creation failed"); return false; }

    gRayTraceCB    = host->CreateConstantBuffer(device, sizeof(RayTraceCBData));
    gShadowDebugCB = host->CreateConstantBuffer(device, sizeof(DebugCBData));
    if (!gRayTraceCB || !gShadowDebugCB) { Log("DustShadowsRT: Phase 2 CB creation failed"); return false; }

    gInitialized = true;
    Log("DustShadowsRT: initialized (Phase 2)");
    return true;
}

void Shutdown()
{
    SAFE_RELEASE(gCountTex); SAFE_RELEASE(gCountUAV); SAFE_RELEASE(gCountSRV);
    SAFE_RELEASE(gDepthTex); SAFE_RELEASE(gDepthDSV); SAFE_RELEASE(gDepthSRV);
    SAFE_RELEASE(gIndicesBuf); SAFE_RELEASE(gIndicesUAV); SAFE_RELEASE(gIndicesSRV);
    SAFE_RELEASE(gPrimBuf);    SAFE_RELEASE(gPrimUAV);    SAFE_RELEASE(gPrimSRV);
    SAFE_RELEASE(gShadowTex);  SAFE_RELEASE(gShadowUAV);  SAFE_RELEASE(gShadowSRV);
    SAFE_RELEASE(gBuildGS);
    SAFE_RELEASE(gBuildPS);
    SAFE_RELEASE(gDebugPS);
    SAFE_RELEASE(gRayTraceCS);
    SAFE_RELEASE(gShadowDebugPS);
    SAFE_RELEASE(gRSNoCull);
    SAFE_RELEASE(gDSSDefault);
    SAFE_RELEASE(gNoBlend);
    SAFE_RELEASE(gPointSampler);
    SAFE_RELEASE(gBuildCB);
    SAFE_RELEASE(gDebugCB);
    SAFE_RELEASE(gDrawOffsetCB);
    SAFE_RELEASE(gRayTraceCB);
    SAFE_RELEASE(gShadowDebugCB);
    SAFE_RELEASE(gSunStagingCB);
    gInitialized = false;
    gShadowMaskReady = false;
    gDpmAllocSize = 0;
    gPrimAllocSize = 0;
    gShadowW = 0; gShadowH = 0;
}

void OnResolutionChanged(ID3D11Device* device, UINT w, UINT h)
{
    gWidth = w;
    gHeight = h;
    if (device && (w != gShadowW || h != gShadowH))
        CreateShadowMaskTextures(device, w, h);
}

void ExtractSunDirection(ID3D11DeviceContext* ctx)
{
    ID3D11Buffer* psCB = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &psCB);
    if (!psCB) return;

    D3D11_BUFFER_DESC desc;
    psCB->GetDesc(&desc);

    if (desc.ByteWidth < 16) { psCB->Release(); return; }

    if (!gSunStagingCB)
    {
        D3D11_BUFFER_DESC sd = desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;

        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (device)
        {
            device->CreateBuffer(&sd, nullptr, &gSunStagingCB);
            device->Release();
        }
    }

    if (!gSunStagingCB) { psCB->Release(); return; }

    ctx->CopyResource(gSunStagingCB, psCB);
    psCB->Release();

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(gSunStagingCB, 0, D3D11_MAP_READ, 0, &m)))
    {
        const float* d = (const float*)m.pData;
        float sd[3] = { d[0], d[1], d[2] };
        float len = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        ctx->Unmap(gSunStagingCB, 0);
        if (len > 0.1f && len < 10.0f)
        {
            gSunDir[0] = sd[0]/len;
            gSunDir[1] = sd[1]/len;
            gSunDir[2] = sd[2]/len;
            bool wasInvalid = !gHasSunDir;
            gHasSunDir = true;
            // Mirror live sun direction into the GUI diagnostics fields.
            gConfig.diagSunX = gSunDir[0];
            gConfig.diagSunY = gSunDir[1];
            gConfig.diagSunZ = gSunDir[2];
            gConfig.diagSunValid = 1;
            if (wasInvalid)
                Log("DustShadowsRT: sun dir captured (%.3f, %.3f, %.3f)", gSunDir[0], gSunDir[1], gSunDir[2]);
        }
    }
}

bool HasValidSunDirection() { return gHasSunDir; }

uint32_t BuildDPM(ID3D11DeviceContext* ctx, ID3D11Device* device,
                  const DustCameraData* camera)
{
    if (!gInitialized || !gConfig.enabled || !gHasSunDir || !camera || !camera->valid)
        return 0;

    // Resize DPM if config changed
    if (gConfig.dpmResolution != gDpmAllocSize)
    {
        if (!CreateDPMResources(device, gConfig.dpmResolution))
            return 0;
        CreatePhase2Resources(device, gConfig.dpmResolution, gConfig.maxPrimBufferSize);
    }
    if (gPrimAllocSize != gConfig.maxPrimBufferSize)
        CreatePhase2Resources(device, gDpmAllocSize, gConfig.maxPrimBufferSize);

    // Build sun VP and its inverse
    float lightVP[16];
    float invLightVP[16];
    ComputeSunVP(gSunDir, camera->camPosition,
                 gConfig.frustumExtent, gConfig.frustumDepth,
                 lightVP, invLightVP);

    // Update build CB
    BuildCBData cb = {};
    memcpy(cb.invLightVP, invLightVP, sizeof(invLightVP));
    cb.dpmSize  = (float)gDpmAllocSize;
    cb.maxDepth = (float)gConfig.maxPrimsPerTexel;
    gHost->UpdateConstantBuffer(ctx, gBuildCB, &cb, sizeof(cb));

    // Save GPU state — we'll touch many bindings
    gHost->SaveState(ctx);

    // Cache VP for RayTrace (called separately)
    memcpy(gLastLightVP, lightVP, 64);
    memcpy(gLastCamPos, camera->camPosition, 12);

    // Clear count map and depth; prim/indices buffers don't need clearing (overwritten up to count)
    UINT zero[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(gCountUAV, zero);
    ctx->ClearDepthStencilView(gDepthDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Bind DSV + 3 UAVs at slots 0..2: count map | indices map | prim buffer
    // UAVStartSlot = 0 (mandatory when NumRTVs = 0, and ensures u0/u1/u2 registers match)
    ID3D11UnorderedAccessView* uavs[3] = { gCountUAV, gIndicesUAV, gPrimUAV };
    UINT initialCounts[3] = { 0, 0, 0 };
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, gDepthDSV,
        0,           // UAVStartSlot = 0: u0=count, u1=indices, u2=prims
        gIndicesUAV ? 3u : 1u,   // fall back to count-only if Phase 2 buffers missing
        uavs, initialCounts);

    D3D11_VIEWPORT vp = {};
    vp.Width  = (float)gDpmAllocSize;
    vp.Height = (float)gDpmAllocSize;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ctx->RSSetState(gRSNoCull);
    ctx->OMSetDepthStencilState(gDSSDefault, 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(gNoBlend, blendFactor, 0xFFFFFFFF);

    ctx->GSSetShader(gBuildGS, nullptr, 0);
    ctx->PSSetShader(gBuildPS, nullptr, 0);
    ctx->GSSetConstantBuffers(0, 1, &gBuildCB);  // slot 0: invLightVP + dpmSize
    ctx->PSSetConstantBuffers(0, 1, &gBuildCB);  // slot 0: same (dpmSize, maxDepth)

    // Pre-draw callback: updates slot 1 CB with drawCallOffset before each draw
    struct DrawOffsetCtx { ID3D11Buffer* cb; const DustHostAPI* host; };
    DrawOffsetCtx cbCtx = { gDrawOffsetCB, gHost };

    auto preDrawCB = [](ID3D11DeviceContext* ctx2, uint32_t /*di*/, uint32_t priorTris, void* ud)
    {
        DrawOffsetCtx* c = static_cast<DrawOffsetCtx*>(ud);
        if (!c->cb) return;
        DrawOffsetCBData off = { priorTris, {0,0,0} };
        c->host->UpdateConstantBuffer(ctx2, c->cb, &off, sizeof(off));
        ctx2->GSSetConstantBuffers(1, 1, &c->cb);
        ctx2->PSSetConstantBuffers(1, 1, &c->cb);
    };

    uint32_t replayed = gHost->ReplayGeometryEx
        ? gHost->ReplayGeometryEx(ctx, device, lightVP, preDrawCB, &cbCtx)
        : gHost->ReplayGeometry(ctx, device, lightVP);  // Phase 1 fallback

    // Unbind all UAVs + GS
    ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 3, nullUAVs, nullptr);
    ctx->GSSetShader(nullptr, nullptr, 0);

    gHost->RestoreState(ctx);

    // Update GUI diagnostics
    uint32_t captured = gHost->GetGeometryDrawCount();
    gConfig.diagCapturedDraws = (int)captured;
    gConfig.diagReplayedDraws = (int)replayed;

    // Histogram by shader category (proxy for topology — TERRAIN is the only tessellated
    // class in Kenshi's GBuffer, all other categories use triangle list/strip). Logged
    // every 128 frames so we can spot when the player walks into a different scene.
    static uint32_t buildFrame = 0;
    buildFrame++;
    if ((buildFrame & 0x7F) == 1)
    {
        uint32_t cats[7] = {0}; // index by DustShaderCategory (0..6)
        for (uint32_t i = 0; i < captured; i++)
        {
            DustGeometryDraw d;
            if (gHost->GetGeometryDrawInfo(i, &d) == 0)
            {
                int c = (int)d.vsCategory;
                if (c >= 0 && c < 7) cats[c]++;
            }
        }
        Log("DustShadowsRT: captures=%u replayed=%u  unknown=%u objects=%u terrain=%u foliage=%u skin=%u triplanar=%u distant_town=%u",
            captured, replayed,
            cats[0], cats[1], cats[2], cats[3], cats[4], cats[5], cats[6]);
        if (cats[2] > 0)
            Log("DustShadowsRT:   note: %u terrain draws use tessellation patches and will not be in DPM (Phase 1 limitation)",
                cats[2]);
    }

    static int firstReplayLog = 0;
    if (firstReplayLog < 3 && replayed > 0)
    {
        Log("DustShadowsRT: BuildDPM replayed %u/%u draws (sun=%.2f,%.2f,%.2f cam=%.1f,%.1f,%.1f)",
            replayed, captured, gSunDir[0], gSunDir[1], gSunDir[2],
            camera->camPosition[0], camera->camPosition[1], camera->camPosition[2]);
        firstReplayLog++;
    }

    return replayed;
}

void RenderDebugHeatmap(ID3D11DeviceContext* ctx,
                        ID3D11RenderTargetView* dstRTV,
                        UINT viewportW, UINT viewportH)
{
    if (!gInitialized || !gConfig.debugView || !dstRTV) return;

    gHost->SaveState(ctx);

    DebugCBData cb = {};
    cb.viewportSize[0] = (float)viewportW;
    cb.viewportSize[1] = (float)viewportH;
    cb.dpmSize = (float)gDpmAllocSize;
    cb.maxDisplay = (float)gConfig.maxPrimsPerTexel;
    gHost->UpdateConstantBuffer(ctx, gDebugCB, &cb, sizeof(cb));

    ctx->OMSetRenderTargets(1, &dstRTV, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)viewportW, (float)viewportH, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    float bf[4] = { 0,0,0,0 };
    ctx->OMSetBlendState(gNoBlend, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(gRSNoCull);

    ctx->PSSetShaderResources(0, 1, &gCountSRV);
    ctx->PSSetSamplers(0, 1, &gPointSampler);
    ctx->PSSetConstantBuffers(0, 1, &gDebugCB);

    gHost->DrawFullscreenTriangle(ctx, gDebugPS);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

void RayTrace(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depthSRV,
              ID3D11ShaderResourceView* normalsSRV,
              const DustCameraData* camera,
              UINT viewportW, UINT viewportH)
{
    if (!gInitialized || !gRayTraceCS || !gShadowUAV) return;
    if (!depthSRV || !camera || !camera->valid) return;

    // Recreate shadow mask if viewport changed
    if (viewportW != gShadowW || viewportH != gShadowH)
    {
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (device)
        {
            CreateShadowMaskTextures(device, viewportW, viewportH);
            device->Release();
        }
        if (!gShadowUAV) return;
    }

    gHost->SaveState(ctx);

    RayTraceCBData cb = {};
    memcpy(cb.inverseView, camera->inverseView, 64);
    memcpy(cb.lightVP, gLastLightVP, 64);
    cb.sunDir[0] = gSunDir[0]; cb.sunDir[1] = gSunDir[1]; cb.sunDir[2] = gSunDir[2];
    cb.tanHalfFov = gConfig.tanHalfFov;
    cb.viewportSize[0] = (float)viewportW;
    cb.viewportSize[1] = (float)viewportH;
    cb.dpmSize = (float)gDpmAllocSize;
    cb.maxDepth = (float)gConfig.maxPrimsPerTexel;
    cb.primBufSize = (uint32_t)gPrimAllocSize;
    gHost->UpdateConstantBuffer(ctx, gRayTraceCB, &cb, sizeof(cb));

    // Clear shadow mask to lit (1 = lit, overwritten by ray test)
    float clearVal[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ctx->ClearUnorderedAccessViewFloat(gShadowUAV, clearVal);

    ctx->CSSetShader(gRayTraceCS, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &gRayTraceCB);

    ID3D11ShaderResourceView* srvs[4] = { depthSRV, normalsSRV, gCountSRV, gIndicesSRV };
    ctx->CSSetShaderResources(0, 4, srvs);

    ID3D11ShaderResourceView* srvPrim = gPrimSRV;
    ctx->CSSetShaderResources(4, 1, &srvPrim);

    ctx->CSSetUnorderedAccessViews(0, 1, &gShadowUAV, nullptr);
    ctx->CSSetSamplers(0, 1, &gPointSampler);

    UINT gx = (viewportW + 7) / 8;
    UINT gy = (viewportH + 7) / 8;
    ctx->Dispatch(gx, gy, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRVs[5] = {};
    ctx->CSSetShaderResources(0, 5, nullSRVs);
    ctx->CSSetShader(nullptr, nullptr, 0);

    gHost->RestoreState(ctx);
    gShadowMaskReady = true;
}

ID3D11ShaderResourceView* GetShadowSRV()    { return gShadowSRV; }
ID3D11SamplerState*       GetShadowSampler(){ return gPointSampler; }
bool                      IsShadowMaskReady(){ return gShadowMaskReady && gShadowSRV; }

void RenderDebugShadowMask(ID3D11DeviceContext* ctx,
                           ID3D11RenderTargetView* dstRTV,
                           UINT viewportW, UINT viewportH)
{
    if (!gInitialized || !gShadowDebugPS || !gShadowSRV || !dstRTV) return;

    gHost->SaveState(ctx);

    DebugCBData cb = {};
    cb.viewportSize[0] = (float)viewportW;
    cb.viewportSize[1] = (float)viewportH;
    cb.dpmSize = (float)gDpmAllocSize;
    cb.maxDisplay = 1.0f;
    gHost->UpdateConstantBuffer(ctx, gShadowDebugCB, &cb, sizeof(cb));

    ctx->OMSetRenderTargets(1, &dstRTV, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)viewportW, (float)viewportH, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    float bf[4] = {};
    ctx->OMSetBlendState(gNoBlend, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(gRSNoCull);

    ctx->PSSetShaderResources(0, 1, &gShadowSRV);
    ctx->PSSetSamplers(0, 1, &gPointSampler);
    ctx->PSSetConstantBuffers(0, 1, &gShadowDebugCB);

    gHost->DrawFullscreenTriangle(ctx, gShadowDebugPS);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    gHost->RestoreState(ctx);
}

Config& GetConfig() { return gConfig; }

} // namespace DPMRenderer
