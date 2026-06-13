// DustSSS.cpp - Bend Studio Screen Space Shadows for Dust (Phase 1).
//
// Directional SUN screen-space shadows via a compute pass (Sony Bend's
// "bend_sss" technique). Each frame at POST_LIGHTING:
//   1. Reads the host's SRV-able copy of the scene depth ("depth_hw").
//   2. Builds the sun's homogeneous light projection from the camera VP and
//      the CSM sun direction.
//   3. Runs Bend::BuildDispatchList() (CPU) to get the compute dispatches.
//   4. Dispatches the ported bend_sss compute shader, writing an R8 shadow
//      buffer (1 = lit, 0 = shadowed).
//   5. Binds that buffer at PS t11/s11 so the patched deferred main_fs can
//      optionally multiply the sun term by it (gated by dustSSSApply at b9).
//
// Phase 1 ships a debug view to validate the raw shadow buffer; the apply-to-sun
// path is OFF by default. Visual tuning happens later with the user.

#include "../../src/DustAPI.h"
#include "DustLog.h"
#include "bend_sss_cpu.h"   // Bend::BuildDispatchList (plain C++)

#include <d3d11.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>

DustLogFn gLogFn = nullptr;

// ==================== Config ====================

struct SSSConfig {
    bool  enabled        = true;
    bool  applyToSun     = false;   // multiply the sun term by the shadow (default OFF)
    int   debugView      = 0;       // 0=Off, 1=Shadow, 2=WaveIndex, 3=EdgeMask
    float surfaceThickness = 0.005f;
    float bilinearThreshold = 0.02f;
    float shadowContrast = 4.0f;
    bool  flipZ          = false;   // swap Far/Near (Kenshi Z convention probe)
};

static SSSConfig gConfig;
static const DustHostAPI* gHost = nullptr;
static HMODULE gPluginModule = nullptr;

// ==================== Compute resources ====================

static ID3D11ComputeShader*       gCS       = nullptr;
static ID3D11Buffer*              gParamsCB = nullptr;
static ID3D11SamplerState*        gBorderSampler = nullptr;  // point, clamp-to-border (= FarDepth)
static ID3D11SamplerState*        gLinearClamp   = nullptr;  // for the deferred read + debug blit

// Output shadow buffer (R8_UNORM), resized to match the depth SRV.
static ID3D11Texture2D*           gOutTex   = nullptr;
static ID3D11UnorderedAccessView* gOutUAV   = nullptr;
static ID3D11ShaderResourceView*  gOutSRV   = nullptr;
static uint32_t                   gOutW = 0, gOutH = 0;

// Debug blit
static ID3D11PixelShader*         gDebugPS  = nullptr;

// Sun-apply gating cbuffer bound at PS slot b9. DustSSS owns this so we don't
// have to touch the Shadows plugin's b7 buffer. The patched deferred main_fs
// reads { float dustSSSApply; float3 pad; } from it and gates the multiply.
static ID3D11Buffer*              gApplyCB  = nullptr;
struct alignas(16) SSSApplyCB { float apply; float pad[3]; };

// GPU timing
static ID3D11Query*               gTsDisjoint = nullptr;
static ID3D11Query*               gTsBegin    = nullptr;
static ID3D11Query*               gTsEnd      = nullptr;
static bool                       gTsPending  = false;
static float                      gGpuMs      = 0.0f;

static std::string gShaderDir;

// b0 cbuffer layout — MUST match cbuffer SSSCB in shaders/sss_cs.hlsl
// (16-byte rows). 96 bytes total.
struct alignas(16) SSSCBData {
    float LightCoordinate[4];           // c0
    int   WaveOffset[2];                // c1.xy
    float InvDepthTextureSize[2];       // c1.zw
    float SurfaceThickness;             // c2.x
    float BilinearThreshold;            // c2.y
    float ShadowContrast;               // c2.z
    float FarDepthValue;                // c2.w
    float NearDepthValue;               // c3.x
    float DepthBounds[2];               // c3.yz
    float _pad0;                        // c3.w
    int   IgnoreEdgePixels;             // c4.x
    int   UsePrecisionOffset;           // c4.y
    int   BilinearSamplingOffsetMode;   // c4.z
    int   DebugOutputEdgeMask;          // c4.w
    int   DebugOutputThreadIndex;       // c5.x
    int   DebugOutputWaveIndex;         // c5.y
    int   _pad1;                        // c5.z
    int   _pad2;                        // c5.w
};

// ==================== Matrix helpers (mirrored from DustShadows.cpp) ====================

static void MatMul4x4(float* out, const float* a, const float* b)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c]
                           + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

// Build the camera View matrix (world->view) row-major from the camera basis,
// so worldPoint(row) * View = viewPoint(row). Matches the deferred convention.
static void BuildViewRowMajor(const DustCameraData& cam, float v[16])
{
    const float* R = cam.camRight; const float* U = cam.camUp;
    const float* F = cam.camForward; const float* P = cam.camPosition;
    float dR = P[0]*R[0] + P[1]*R[1] + P[2]*R[2];
    float dU = P[0]*U[0] + P[1]*U[1] + P[2]*U[2];
    float dF = P[0]*F[0] + P[1]*F[1] + P[2]*F[2];
    v[0]=R[0]; v[1]=U[0]; v[2]=F[0]; v[3]=0;
    v[4]=R[1]; v[5]=U[1]; v[6]=F[1]; v[7]=0;
    v[8]=R[2]; v[9]=U[2]; v[10]=F[2]; v[11]=0;
    v[12]=-dR; v[13]=-dU; v[14]=-dF; v[15]=1;
}

// Row-vector x row-major matrix: out[c] = sum_r v[r] * m[r*4+c].
// This is the convention Bend documents for inLightProjection:
//   inLightProjection = float4(direction, 0) * ViewProjectionMatrix
static void VecMatRowMajor(float out[4], const float v[4], const float m[16])
{
    for (int c = 0; c < 4; c++)
        out[c] = v[0]*m[0*4+c] + v[1]*m[1*4+c] + v[2]*m[2*4+c] + v[3]*m[3*4+c];
}

// ==================== Plugin dir ====================

static std::string GetPluginDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(gPluginModule, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// ==================== Output texture (re)creation ====================

static void ReleaseOutput()
{
    if (gOutSRV) { gOutSRV->Release(); gOutSRV = nullptr; }
    if (gOutUAV) { gOutUAV->Release(); gOutUAV = nullptr; }
    if (gOutTex) { gOutTex->Release(); gOutTex = nullptr; }
    gOutW = gOutH = 0;
}

static bool CreateOutput(ID3D11Device* device, uint32_t w, uint32_t h)
{
    ReleaseOutput();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &gOutTex))) return false;
    if (FAILED(device->CreateUnorderedAccessView(gOutTex, nullptr, &gOutUAV))) return false;
    if (FAILED(device->CreateShaderResourceView(gOutTex, nullptr, &gOutSRV))) return false;
    gOutW = w; gOutH = h;
    Log("SSS: output shadow buffer %ux%u (R8_UNORM)", w, h);
    return true;
}

// Query the dimensions of the resource backing an SRV.
static bool GetSRVTexSize(ID3D11ShaderResourceView* srv, uint32_t& w, uint32_t& h)
{
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return false;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr) || !tex) return false;
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    tex->Release();
    w = td.Width; h = td.Height;
    return (w > 0 && h > 0);
}

// ==================== Debug blit PS ====================

static const char* kDebugPS =
    "Texture2D<float> gShadow : register(t0);\n"
    "SamplerState     gSamp   : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float s = gShadow.SampleLevel(gSamp, uv, 0);\n"
    "  return float4(s, s, s, 1);\n"
    "}\n";

// ==================== Init / Shutdown ====================

static int SSSInit(ID3D11Device* device, uint32_t w, uint32_t h, const DustHostAPI* host)
{
#undef Log
    gLogFn = host->Log;
#define Log DustLog
    gHost = host;
    gShaderDir = GetPluginDir() + "\\shaders\\";

    // Tell the host to start copying the scene depth into the SRV-able
    // "depth_hw" texture (the SSS compute pass reads it).
    if (host->SetSceneDepthCapture)
        host->SetSceneDepthCapture(1);

    // Compile the compute shader from file (resolves #include "bend_sss_gpu.hlsli").
    ID3DBlob* csBlob = host->CompileShaderFromFile
        ? host->CompileShaderFromFile((gShaderDir + "sss_cs.hlsl").c_str(), "main", "cs_5_0")
        : nullptr;
    if (!csBlob) { Log("SSS: compute shader compile failed"); return -1; }
    HRESULT hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &gCS);
    csBlob->Release();
    if (FAILED(hr)) { Log("SSS: CreateComputeShader failed (0x%08X)", (unsigned)hr); return -1; }

    // Debug-view blit PS.
    ID3DBlob* psBlob = host->CompileShader ? host->CompileShader(kDebugPS, "main", "ps_5_0") : nullptr;
    if (psBlob)
    {
        device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &gDebugPS);
        psBlob->Release();
    }

    gParamsCB = host->CreateConstantBuffer(device, sizeof(SSSCBData));
    if (!gParamsCB) { Log("SSS: params CB create failed"); return -1; }

    gApplyCB = host->CreateConstantBuffer(device, sizeof(SSSApplyCB));
    if (!gApplyCB) { Log("SSS: apply CB create failed"); return -1; }

    // Border sampler: point, clamp-to-border with border = 0 (= FarDepthValue),
    // so off-screen reads return far depth (no spurious shadows from offscreen).
    // NOTE the sampler-desc gotcha: ComparisonFunc and MaxLOD MUST be set even
    // for a non-comparison sampler, or CreateSamplerState fails on the debug runtime.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.0f;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &gBorderSampler)))
    { Log("SSS: border sampler create failed"); return -1; }

    // Linear-clamp sampler for the deferred read of the shadow buffer + debug blit.
    D3D11_SAMPLER_DESC ld = {};
    ld.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ld.AddressU = ld.AddressV = ld.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ld.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ld.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&ld, &gLinearClamp)))
    { Log("SSS: linear sampler create failed"); return -1; }

    // GPU timestamp queries (non-fatal if unavailable).
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; device->CreateQuery(&qd, &gTsDisjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;          device->CreateQuery(&qd, &gTsBegin);
    qd.Query = D3D11_QUERY_TIMESTAMP;          device->CreateQuery(&qd, &gTsEnd);

    (void)w; (void)h;
    Log("SSS: initialized");
    return 0;
}

static void SSSShutdown()
{
    ReleaseOutput();
    if (gTsEnd)        { gTsEnd->Release();        gTsEnd = nullptr; }
    if (gTsBegin)      { gTsBegin->Release();      gTsBegin = nullptr; }
    if (gTsDisjoint)   { gTsDisjoint->Release();   gTsDisjoint = nullptr; }
    if (gDebugPS)      { gDebugPS->Release();      gDebugPS = nullptr; }
    if (gLinearClamp)  { gLinearClamp->Release();  gLinearClamp = nullptr; }
    if (gBorderSampler){ gBorderSampler->Release(); gBorderSampler = nullptr; }
    if (gApplyCB)      { gApplyCB->Release();      gApplyCB = nullptr; }
    if (gParamsCB)     { gParamsCB->Release();     gParamsCB = nullptr; }
    if (gCS)           { gCS->Release();           gCS = nullptr; }
    if (gHost && gHost->SetSceneDepthCapture)
        gHost->SetSceneDepthCapture(0);
    Log("SSS: shut down");
}

static void SSSOnResolutionChanged(ID3D11Device* device, uint32_t w, uint32_t h)
{
    (void)device; (void)w; (void)h;
    ReleaseOutput();  // recreated next frame at the live depth size
}

// ==================== Per-frame compute pass ====================

static void SSSPreExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    if (!gConfig.enabled || !gCS || !gParamsCB) return;

    // Bind the apply gate at b9 with apply=0 first. If the compute pass below
    // succeeds we re-bind with the real value. This guarantees the deferred sun
    // draw never multiplies by a stale/garbage shadow buffer on a frame the pass
    // couldn't run (no depth_hw yet, sun not captured, etc.).
    if (gApplyCB)
    {
        SSSApplyCB acb = {}; acb.apply = 0.0f;
        host->UpdateConstantBuffer(ctx->context, gApplyCB, &acb, sizeof(acb));
        ctx->context->PSSetConstantBuffers(9, 1, &gApplyCB);
    }

    ID3D11ShaderResourceView* srcDepth = host->GetSRV ? host->GetSRV("depth_hw") : nullptr;
    if (!srcDepth) return;   // host hasn't populated the copy yet

    uint32_t w = 0, h = 0;
    if (!GetSRVTexSize(srcDepth, w, h)) return;
    if (w != gOutW || h != gOutH || !gOutTex)
        if (!CreateOutput(ctx->device, w, h)) return;

    if (!ctx->camera.valid) return;

    // Sun direction from the CSM snapshot.
    DustCSMData csm = {};
    bool haveCsm = host->GetCSMData && host->GetCSMData(&csm) != 0 && csm.valid;
    if (!haveCsm) return;
    float sx = csm.sunDirection[0], sy = csm.sunDirection[1], sz = csm.sunDirection[2];
    float slen = sqrtf(sx*sx + sy*sy + sz*sz);
    if (slen < 1e-6f) return;   // sun direction not yet captured
    sx /= slen; sy /= slen; sz /= slen;

    // Camera View*Proj (row-major), same construction DustShadows uses for the
    // back-face replay VP that ReplayGeometry consumes.
    float view[16], vp[16];
    BuildViewRowMajor(ctx->camera, view);
    MatMul4x4(vp, view, ctx->camera.projMatrix);

    // inLightProjection = float4(dir, 0) * ViewProjectionMatrix (row-vector convention).
    float dir4[4] = { sx, sy, sz, 0.0f };
    float inLightProjection[4];
    VecMatRowMajor(inLightProjection, dir4, vp);

    int viewport[2] = { (int)w, (int)h };
    int minB[2] = { 0, 0 };
    int maxB[2] = { (int)w, (int)h };
    Bend::DispatchList list = Bend::BuildDispatchList(inLightProjection, viewport, minB, maxB, false, 64);

    // ---- GPU timing readback (previous frame) ----
    ID3D11DeviceContext* dc = ctx->context;
    if (gTsPending && gTsDisjoint)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (dc->GetData(gTsDisjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            UINT64 t0 = 0, t1 = 0;
            if (dc->GetData(gTsBegin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                dc->GetData(gTsEnd,   &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
            {
                gTsPending = false;
                if (!dj.Disjoint && dj.Frequency)
                    gGpuMs = (float)(double(t1 - t0) / double(dj.Frequency) * 1000.0);
            }
        }
    }
    bool doTime = (!gTsPending && gTsDisjoint && gTsBegin && gTsEnd);

    // ---- Fill the (mostly constant) part of the CB ----
    SSSCBData cb = {};
    cb.LightCoordinate[0] = list.LightCoordinate_Shader[0];
    cb.LightCoordinate[1] = list.LightCoordinate_Shader[1];
    cb.LightCoordinate[2] = list.LightCoordinate_Shader[2];
    cb.LightCoordinate[3] = list.LightCoordinate_Shader[3];
    cb.InvDepthTextureSize[0] = 1.0f / (float)w;
    cb.InvDepthTextureSize[1] = 1.0f / (float)h;
    cb.SurfaceThickness  = gConfig.surfaceThickness;
    cb.BilinearThreshold = gConfig.bilinearThreshold;
    cb.ShadowContrast    = gConfig.shadowContrast;
    // Kenshi uses non-linear D32 depth. Default: Far=0, Near=1 (Bend's typical).
    // "Flip Z" swaps these for the reversed-Z convention probe.
    cb.FarDepthValue  = gConfig.flipZ ? 1.0f : 0.0f;
    cb.NearDepthValue = gConfig.flipZ ? 0.0f : 1.0f;
    cb.DepthBounds[0] = 0.0f;
    cb.DepthBounds[1] = 1.0f;
    cb.IgnoreEdgePixels = 0;
    cb.UsePrecisionOffset = 0;
    cb.BilinearSamplingOffsetMode = 0;
    cb.DebugOutputEdgeMask    = (gConfig.debugView == 3) ? 1 : 0;
    cb.DebugOutputThreadIndex = 0;
    cb.DebugOutputWaveIndex   = (gConfig.debugView == 2) ? 1 : 0;

    host->SaveState(dc);
    if (doTime) { dc->Begin(gTsDisjoint); dc->End(gTsBegin); }

    dc->CSSetShader(gCS, nullptr, 0);
    dc->CSSetShaderResources(0, 1, &srcDepth);
    dc->CSSetSamplers(0, 1, &gBorderSampler);
    UINT initCount = (UINT)-1;
    dc->CSSetUnorderedAccessViews(0, 1, &gOutUAV, &initCount);

    for (int i = 0; i < list.DispatchCount; i++)
    {
        const Bend::DispatchData& d = list.Dispatch[i];
        cb.WaveOffset[0] = d.WaveOffset_Shader[0];
        cb.WaveOffset[1] = d.WaveOffset_Shader[1];
        host->UpdateConstantBuffer(dc, gParamsCB, &cb, sizeof(cb));
        dc->CSSetConstantBuffers(0, 1, &gParamsCB);
        dc->Dispatch((UINT)d.WaveCount[0], (UINT)d.WaveCount[1], (UINT)d.WaveCount[2]);
    }

    if (doTime) { dc->End(gTsEnd); dc->End(gTsDisjoint); gTsPending = true; }

    // Null-unbind CS stage: SaveState/RestoreState do NOT cover compute bindings.
    ID3D11ComputeShader*       nullCS  = nullptr;
    ID3D11ShaderResourceView*  nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11Buffer*              nullCB  = nullptr;
    ID3D11SamplerState*        nullSamp = nullptr;
    dc->CSSetUnorderedAccessViews(0, 1, &nullUAV, &initCount);
    dc->CSSetShaderResources(0, 1, &nullSRV);
    dc->CSSetConstantBuffers(0, 1, &nullCB);
    dc->CSSetSamplers(0, 1, &nullSamp);
    dc->CSSetShader(nullCS, nullptr, 0);

    host->RestoreState(dc);

    // Bind the shadow buffer at PS t11 + sampler s11 and LEAVE bound, so the
    // patched deferred main_fs sun pass (and the debug blit) can read it.
    if (gOutSRV)
    {
        dc->PSSetShaderResources(11, 1, &gOutSRV);
        dc->PSSetSamplers(11, 1, &gLinearClamp);
    }

    // Now the shadow buffer is valid for this frame: enable the sun multiply if
    // requested. Debug view forces apply off (we don't want a debug-only frame
    // to also dim the lit scene). Re-bind b9 with the real value.
    if (gApplyCB)
    {
        SSSApplyCB acb = {};
        acb.apply = (gConfig.applyToSun && gConfig.debugView == 0 && gOutSRV) ? 1.0f : 0.0f;
        host->UpdateConstantBuffer(dc, gApplyCB, &acb, sizeof(acb));
        dc->PSSetConstantBuffers(9, 1, &gApplyCB);
    }
}

static void SSSPostExecute(const DustFrameContext* ctx, const DustHostAPI* host)
{
    // Debug view: blit the raw R8 shadow buffer over the current RT so it's
    // visible. The shadow / wave-index / edge-mask content is selected via the
    // compute-shader debug flags (debugView 1 = the plain shadow result).
    if (!gConfig.enabled || gConfig.debugView == 0 || !gDebugPS || !gOutSRV) return;

    host->SaveState(ctx->context);
    ID3D11DeviceContext* dc = ctx->context;
    dc->PSSetShaderResources(0, 1, &gOutSRV);
    dc->PSSetSamplers(0, 1, &gLinearClamp);
    host->DrawFullscreenTriangle(dc, gDebugPS);
    host->RestoreState(ctx->context);
}

static int SSSIsEnabled() { return gConfig.enabled ? 1 : 0; }

// ==================== GUI ====================

static const char* const kDebugViewLabels[] = { "Off", "Shadow", "Wave Index", "Edge Mask", nullptr };

static DustSettingDesc gSettings[] = {
    { "Screen Space Shadows", DUST_SETTING_SECTION, nullptr, 0.0f, 0.0f, nullptr, nullptr, nullptr, DUST_PERF_NONE },
    { "Enabled",        DUST_SETTING_BOOL,  &gConfig.enabled,    0.0f, 1.0f,  "Enabled",        nullptr, "Run the Bend screen-space sun-shadow compute pass each frame.", DUST_PERF_HIGH },
    { "Apply To Sun",   DUST_SETTING_BOOL,  &gConfig.applyToSun, 0.0f, 1.0f,  "ApplyToSun",     nullptr, "Multiply the deferred sun light by the screen-space shadow. Off by default (validate with Debug View first).", DUST_PERF_NONE },
    { "Debug View",     DUST_SETTING_ENUM,  &gConfig.debugView,  0.0f, 3.0f,  "DebugView",      kDebugViewLabels, "Blit the raw compute output to screen. Shadow = the shadow term; Wave Index / Edge Mask = Bend tuning visualizations.", DUST_PERF_NONE },
    { "Surface Thickness", DUST_SETTING_FLOAT, &gConfig.surfaceThickness, 0.0001f, 0.05f, "SurfaceThickness", nullptr, "Assumed pixel thickness for shadow casting, as a fraction of non-linear depth. Start 0.005; scale in multiples of 2.", DUST_PERF_NONE },
    { "Bilinear Threshold", DUST_SETTING_FLOAT, &gConfig.bilinearThreshold, 0.001f, 0.2f, "BilinearThreshold", nullptr, "Depth-difference threshold for edge detection. Use Debug View = Edge Mask to tune. Scale alongside Surface Thickness.", DUST_PERF_NONE },
    { "Shadow Contrast", DUST_SETTING_FLOAT, &gConfig.shadowContrast, 1.0f, 8.0f, "ShadowContrast", nullptr, "Contrast boost on the shadow transition. Values >= 1 valid; start 4.", DUST_PERF_NONE },
    { "Flip Z",         DUST_SETTING_BOOL,  &gConfig.flipZ,      0.0f, 1.0f,  "FlipZ",          nullptr, "Swap the Far/Near depth values. Toggle if shadows look inverted or like a paper cutout (Kenshi's depth-buffer Z convention is uncertain).", DUST_PERF_NONE },
    // Note: Sample Count (60), Hard Shadow Samples (4), Fade Out Samples (8) are
    // compile-time #defines in shaders/sss_cs.hlsl and are not exposed here.
};

extern "C" __declspec(dllexport) int DustEffectCreate(DustEffectDesc* desc)
{
    if (!desc) return -1;
    memset(desc, 0, sizeof(*desc));

    desc->apiVersion          = DUST_API_VERSION;
    desc->name                = "Screen Space Shadows";
    desc->configSection       = "SSS";   // keep INI section stable after the display-name rename
    desc->injectionPoint      = DUST_INJECT_POST_LIGHTING;
    // Run before the Shadows plugin (priority -10) so depth_hw is requested and
    // the shadow buffer is bound before the deferred sun draw consumes it. The
    // host populates depth_hw earlier in the frame regardless; this just orders
    // our t11 bind relative to other POST_LIGHTING effects.
    desc->priority            = -20;
    desc->Init                = SSSInit;
    desc->Shutdown            = SSSShutdown;
    desc->OnResolutionChanged = SSSOnResolutionChanged;
    desc->preExecute          = SSSPreExecute;
    desc->postExecute         = SSSPostExecute;
    desc->IsEnabled           = SSSIsEnabled;
    desc->settings            = gSettings;
    desc->settingCount        = sizeof(gSettings) / sizeof(gSettings[0]);
    desc->flags               = DUST_FLAG_FRAMEWORK_CONFIG;
    desc->gpuTimeMsPtr        = &gGpuMs;
    desc->configSection       = "SSS";
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        gPluginModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
