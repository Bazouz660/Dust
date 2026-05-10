#include "TerrainTess.h"
#include "ShaderDatabase.h"
#include "D3D11Hook.h"
#include "DustLog.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <vector>
#include <unordered_map>

namespace TerrainTess
{

namespace {

// ============================================================================
// HLSL — HS / DS
// ============================================================================
//
// VS output layout (from Kenshi's terrain.hlsl main_vs TEXTURED variant +
// our Dust patch that adds wvpCol0/1/2 for sub-vertex displacement):
//   POSITION  : float4 (clip-space pos from worldViewProjMatrix)
//   TEXCOORD0 : float3 (world-space normal)
//   TEXCOORD1 : float3 (world-space position)
//   TEXCOORD2 : float3 (terrain triplanar/horizontal UV)
//   TEXCOORD3 : float4 (overlay & biome map UVs)
//   TEXCOORD4 : float2 (cliff blend weights)
//   TEXCOORD5 : float4 (vertical-cliff distortion offsets)
//   TEXCOORD6/7/8 : float4 (wvpCol1/0/2 — Y/X/Z columns of the WVP matrix,
//                  used for h * wvpCol* clip-space displacement).

static const char* kPassthroughHS = R"HLSL(
struct VsOut
{
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 tex0     : TEXCOORD2;
    float4 tex1     : TEXCOORD3;
    float2 uvblend  : TEXCOORD4;
    float4 texV     : TEXCOORD5;
    float4 wvpCol1  : TEXCOORD6;
    float4 wvpCol0  : TEXCOORD7;
    float4 wvpCol2  : TEXCOORD8;
};

struct HsConst
{
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

cbuffer TessControl : register(b1)
{
    float gMaxFactor;
    float gFactFadeStart;
    float gFactFadeEnd;
    float gAmplitude;
    float gAmpFadeStart;
    float gAmpFadeEnd;
    float gAmpFadeEnabled;
    float gDebugViewMode;
    float gDisplacementBias;
    float gFactorSnapStep;
    float gDispDirWorldUp;
    float gWireframeMode;
    float gSharpMip;
    float gScale;
    float gHfWeight;
    float _gPad0;
    float4 gBlend1Mask;
    float4 gBlend2Mask;
    float4 gBlend3Mask;
};

HsConst HsConstFn(InputPatch<VsOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    float w0 = patch[0].pos.w;
    float w1 = patch[1].pos.w;
    float w2 = patch[2].pos.w;
    float wEdge0 = (w1 + w2) * 0.5;
    float wEdge1 = (w0 + w2) * 0.5;
    float wEdge2 = (w0 + w1) * 0.5;

    HsConst c;
    float e0 = lerp(gMaxFactor, 1.0, smoothstep(gFactFadeStart, gFactFadeEnd, wEdge0));
    float e1 = lerp(gMaxFactor, 1.0, smoothstep(gFactFadeStart, gFactFadeEnd, wEdge1));
    float e2 = lerp(gMaxFactor, 1.0, smoothstep(gFactFadeStart, gFactFadeEnd, wEdge2));
    // Snap to factorSnapStep multiples so per-frame depth jitter doesn't
    // hop tess vertices in and out. Adjacent patches sharing an edge see the
    // same wEdge values and snap to the same factor → no T-junctions.
    float snap = max(1.0, gFactorSnapStep);
    c.edges[0] = max(1.0, round(e0 / snap) * snap);
    c.edges[1] = max(1.0, round(e1 / snap) * snap);
    c.edges[2] = max(1.0, round(e2 / snap) * snap);
    c.inside   = (c.edges[0] + c.edges[1] + c.edges[2]) / 3.0;
    return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HsConstFn")]
[maxtessfactor(64.0)]
VsOut main(InputPatch<VsOut, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}
)HLSL";

// DS replicates the PS BLEND0+1+2+3 chain to compute displacement = visible
// luminance. Verified pixel-exact via the PS-side debug overlay (PatchTerrainShaderForHeightDebug).
static const char* kPassthroughDS = R"HLSL(
struct VsOut
{
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 tex0     : TEXCOORD2;
    float4 tex1     : TEXCOORD3;
    float2 uvblend  : TEXCOORD4;
    float4 texV     : TEXCOORD5;
    float4 wvpCol1  : TEXCOORD6;
    float4 wvpCol0  : TEXCOORD7;
    float4 wvpCol2  : TEXCOORD8;
};

struct HsConst
{
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

struct DsOut
{
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 tex0     : TEXCOORD2;
    float4 tex1     : TEXCOORD3;
    float2 uvblend  : TEXCOORD4;
    float4 texV     : TEXCOORD5;
};

// Live PS textures mirrored per draw. Slot numbering is DS-local, not the PS
// slot — see Begin() for the mirror map.
Texture2D      overlayMap   : register(t0);
Texture2DArray diffuseMaps  : register(t1);
Texture2D      colourMap    : register(t2);
Texture2DArray diffuseMaps1 : register(t3);
Texture2DArray diffuseMaps2 : register(t4);
Texture2DArray diffuseMaps3 : register(t5);
Texture2D      blendMap     : register(t6);
SamplerState   linearWrap   : register(s0);

// Mirror of PS $Globals cbuffer. Offsets verified by reflection at runtime
// (see Dust log "reflecting bound PS" lines after first terrain draw).
// Each BLEND# layer occupies 160 bytes (10 × float4) starting at 48/224/384/544.
cbuffer PsTerrainCb : register(b0)
{
    float4 gPsViewport      : packoffset(c0);
    float4 gPsFarClipCamPos : packoffset(c1);
    float4 gPsWaterWetness  : packoffset(c2);
    // BLEND0 (offsets 48..207).
    float4 gPsScalesA       : packoffset(c3);
    float4 gPsScalesB       : packoffset(c4);
    float4 gPsScalesC       : packoffset(c5);
    float4 gPsSlopeMin      : packoffset(c6);
    float4 gPsSlopeMax      : packoffset(c7);
    float4 gPsSlopeBlend    : packoffset(c8);
    float4 gPsOverlayMult   : packoffset(c9);
    float4 gPsTextureFade   : packoffset(c10);
    float4 gPsAbsorbance0   : packoffset(c11);
    float4 gPsAbsorbance1   : packoffset(c12);
    float4 gPsBrightnessFix : packoffset(c13);
    // BLEND1 (offsets 224..383).
    float4 gPsScalesA1      : packoffset(c14);
    float4 gPsScalesB1      : packoffset(c15);
    float4 gPsScalesC1      : packoffset(c16);
    float4 gPsSlopeMin1     : packoffset(c17);
    float4 gPsSlopeMax1     : packoffset(c18);
    float4 gPsSlopeBlend1   : packoffset(c19);
    float4 gPsOverlayMult1  : packoffset(c20);
    // BLEND2 (offsets 384..543).
    float4 gPsScalesA2      : packoffset(c24);
    float4 gPsScalesB2      : packoffset(c25);
    float4 gPsScalesC2      : packoffset(c26);
    float4 gPsSlopeMin2     : packoffset(c27);
    float4 gPsSlopeMax2     : packoffset(c28);
    float4 gPsSlopeBlend2   : packoffset(c29);
    float4 gPsOverlayMult2  : packoffset(c30);
    // BLEND3 (offsets 544..703).
    float4 gPsScalesA3      : packoffset(c34);
    float4 gPsScalesB3      : packoffset(c35);
    float4 gPsScalesC3      : packoffset(c36);
    float4 gPsSlopeMin3     : packoffset(c37);
    float4 gPsSlopeMax3     : packoffset(c38);
    float4 gPsSlopeBlend3   : packoffset(c39);
    float4 gPsOverlayMult3  : packoffset(c40);
};

cbuffer TessControl : register(b1)
{
    float gMaxFactor;
    float gFactFadeStart;
    float gFactFadeEnd;
    float gAmplitude;
    float gAmpFadeStart;
    float gAmpFadeEnd;
    float gAmpFadeEnabled;
    float gDebugViewMode;
    float gDisplacementBias;
    float gFactorSnapStep;
    float gDispDirWorldUp;
    float gWireframeMode;
    // Bandpass tap mips: sharp tap at gSharpMip, mid at +2, blurry at +4.
    float gSharpMip;
    // Soft-saturation knee: shaped = bp / (|bp| + gScale).
    float gScale;
    // Frequency-falloff weight on the high-freq slice. 1=flat response, 0=mid only.
    float gHfWeight;
    float _gPad0;
    // Per-PS BLEND# channel selectors. Each PS variant uses BLEND1/2/3
    // #defines to pick a channel of blendMap; host fills these per-draw.
    // (1,0,0,0)=R, (0,1,0,0)=G, etc. All-zero = layer inactive.
    float4 gBlend1Mask;
    float4 gBlend2Mask;
    float4 gBlend3Mask;
};

float Lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// One BLEND layer's albedo. Mirrors computeBiome from terrainfp4.hlsl,
// minus the normal/absorbance branches and distance fadeout (irrelevant
// for displacement amplitude). mipLevel overrides the explicit LOD on
// every diffuse sample (DS has no ddx/ddy → must use SampleLevel anyway).
float3 ComputeBiomeAlbedo(Texture2DArray dmap, float3 tc, float2 cliffBlend,
                          float4 weights, float4 omap, float3 colour,
                          float4 sA, float4 sB, float4 sC, float4 oMult,
                          float mipLevel)
{
    const float3 white = float3(1, 1, 1);
    float3 cB  = dmap.SampleLevel(linearWrap, float3(tc.xy * sB.xy, 0.0), mipLevel).rgb * colour;
    float3 cS  = dmap.SampleLevel(linearWrap, float3(tc.xy * sA.xy, 1.0), mipLevel).rgb * colour;
    float3 cG  = dmap.SampleLevel(linearWrap, float3(tc.xy * sB.zw, 3.0), mipLevel).rgb * lerp(white, colour, oMult.y);
    float3 cD  = dmap.SampleLevel(linearWrap, float3(tc.xy * sC.xy, 4.0), mipLevel).rgb * lerp(white, colour, oMult.z);
    float3 cR  = dmap.SampleLevel(linearWrap, float3(tc.xy * sC.zw, 5.0), mipLevel).rgb * lerp(white, colour, oMult.w);
    float3 cCx = dmap.SampleLevel(linearWrap, float3(tc.yz * sA.zw, 2.0), mipLevel).rgb;
    float3 cCz = dmap.SampleLevel(linearWrap, float3(tc.xz * sA.zw, 2.0), mipLevel).rgb;
    float3 cC  = (cCx * cliffBlend.x + cCz * cliffBlend.y) * lerp(white, colour, oMult.x);
    float3 a = lerp(cB, cG, omap.r);
    a = lerp(a, cS, weights.x);
    a = lerp(a, cD, omap.b);
    a = lerp(a, cR, omap.a);
    a = lerp(a, cC, weights.y);
    return a;
}

// PS-visible luminance via the SAME formula the PS uses (BLEND0 + BLEND1/2/3
// chain with channel-masked blendMap weights). Both sides of any chunk
// boundary compute the same value (since the PS does, by construction) →
// displacement is continuous across boundaries → no mesh seams.
// mipLevel is forwarded to the diffuse samples only — colourMap / blendMap /
// overlayMap stay at mip 0 because they're already low-frequency and we want
// per-pixel weights to remain crisp.
float ComputePsLum(float3 tex0, float2 cliffBlend, float4 mapCoords,
                   float3 normal, float3 texV, float mipLevel)
{
    float slope = 1.0 - normalize(normal).y;
    float4 omap = overlayMap.SampleLevel(linearWrap, mapCoords.xy, 0).rgba;
    omap.r = max(omap.r, omap.g);
    float3 colour = colourMap.SampleLevel(linearWrap, mapCoords.xy, 0).rgb * 1.2;

    float4 w0 = smoothstep(gPsSlopeMin - gPsSlopeBlend, gPsSlopeMin, slope)
              * smoothstep(gPsSlopeMax + gPsSlopeBlend, gPsSlopeMax, slope);
    float3 a = ComputeBiomeAlbedo(diffuseMaps, tex0, cliffBlend, w0, omap, colour,
                                  gPsScalesA, gPsScalesB, gPsScalesC, gPsOverlayMult, mipLevel);
    a *= gPsBrightnessFix.x;

    float4 bw = blendMap.SampleLevel(linearWrap, mapCoords.zw, 0);
    float w1 = dot(bw, gBlend1Mask);
    float w2 = dot(bw, gBlend2Mask);
    float w3 = dot(bw, gBlend3Mask);
    a *= (1.0 - w1 - w2 - w3);

    if (w1 > 1e-5)
    {
        float3 tc1 = float3(tex0.xy, texV.x);
        float4 w1w = smoothstep(gPsSlopeMin1 - gPsSlopeBlend1, gPsSlopeMin1, slope)
                   * smoothstep(gPsSlopeMax1 + gPsSlopeBlend1, gPsSlopeMax1, slope);
        float3 a1 = ComputeBiomeAlbedo(diffuseMaps1, tc1, cliffBlend, w1w, omap, colour,
                                       gPsScalesA1, gPsScalesB1, gPsScalesC1, gPsOverlayMult1, mipLevel);
        a += a1 * gPsBrightnessFix.y * w1;
    }
    if (w2 > 1e-5)
    {
        float3 tc2 = float3(tex0.xy, texV.y);
        float4 w2w = smoothstep(gPsSlopeMin2 - gPsSlopeBlend2, gPsSlopeMin2, slope)
                   * smoothstep(gPsSlopeMax2 + gPsSlopeBlend2, gPsSlopeMax2, slope);
        float3 a2 = ComputeBiomeAlbedo(diffuseMaps2, tc2, cliffBlend, w2w, omap, colour,
                                       gPsScalesA2, gPsScalesB2, gPsScalesC2, gPsOverlayMult2, mipLevel);
        a += a2 * gPsBrightnessFix.z * w2;
    }
    if (w3 > 1e-5)
    {
        float3 tc3 = float3(tex0.xy, texV.z);
        float4 w3w = smoothstep(gPsSlopeMin3 - gPsSlopeBlend3, gPsSlopeMin3, slope)
                   * smoothstep(gPsSlopeMax3 + gPsSlopeBlend3, gPsSlopeMax3, slope);
        float3 a3 = ComputeBiomeAlbedo(diffuseMaps3, tc3, cliffBlend, w3w, omap, colour,
                                       gPsScalesA3, gPsScalesB3, gPsScalesC3, gPsOverlayMult3, mipLevel);
        a += a3 * gPsBrightnessFix.w * w3;
    }
    return Lum(a);
}

[domain("tri")]
DsOut main(HsConst c, float3 bary : SV_DomainLocation, const OutputPatch<VsOut, 3> patch)
{
    DsOut o;
    o.normal   = patch[0].normal   * bary.x + patch[1].normal   * bary.y + patch[2].normal   * bary.z;
    o.worldPos = patch[0].worldPos * bary.x + patch[1].worldPos * bary.y + patch[2].worldPos * bary.z;
    o.tex0     = patch[0].tex0     * bary.x + patch[1].tex0     * bary.y + patch[2].tex0     * bary.z;
    o.tex1     = patch[0].tex1     * bary.x + patch[1].tex1     * bary.y + patch[2].tex1     * bary.z;
    o.uvblend  = patch[0].uvblend  * bary.x + patch[1].uvblend  * bary.y + patch[2].uvblend  * bary.z;
    o.texV     = patch[0].texV     * bary.x + patch[1].texV     * bary.y + patch[2].texV     * bary.z;

    float4 wvpCol1 = patch[0].wvpCol1;
    float4 passClip = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
    float ampScale = lerp(1.0, 1.0 - smoothstep(gAmpFadeStart, gAmpFadeEnd, passClip.w), gAmpFadeEnabled);

    // Early-out: skip the BLEND chain entirely when amplitude is essentially
    // zero (e.g. distant chunks faded out, or amplitude slider at 0).
    float ampScaledTotal = gAmplitude * ampScale;
    if (ampScaledTotal < 1e-4)
    {
        o.pos = passClip;
        return o;
    }

    // 4-tap multi-band bandpass with frequency-falloff curve. Replicas at
    // mips K, K+2, K+4, K+8 split the visible-luminance spectrum into three
    // slices spanning ~8x in spatial scale (vs the previous 4x):
    //   slice_hi  = v0 - v1    → highest-freq band (where spikes live)
    //   slice_mid = v1 - v2    → mid-freq band     (where most bumps live)
    //   slice_lo  = v2 - v3    → low-freq band     (where dune-scale features live)
    // Dune-scale features need slice_lo because they're too smooth to register
    // strongly in the previous K..K+4 band — both mid and blurry samples
    // captured them equally and cancelled out. All four taps are per-pixel
    // continuous → seam-safe.
    float v0 = ComputePsLum(o.tex0, o.uvblend, o.tex1, o.normal, o.texV.xyz, gSharpMip);
    float v1 = ComputePsLum(o.tex0, o.uvblend, o.tex1, o.normal, o.texV.xyz, gSharpMip + 2.0);
    float v2 = ComputePsLum(o.tex0, o.uvblend, o.tex1, o.normal, o.texV.xyz, gSharpMip + 4.0);
    float v3 = ComputePsLum(o.tex0, o.uvblend, o.tex1, o.normal, o.texV.xyz, gSharpMip + 8.0);

    // Normalize by colourMap luminance so dark biomes don't shrink the
    // bandpass amplitude (visible_lum scales with colour_lum on both terms).
    float refLum = max(Lum(colourMap.SampleLevel(linearWrap, o.tex1.xy, 0).rgb * 1.2), 0.05);

    float slice_hi  = v0 - v1;
    float slice_mid = v1 - v2;
    float slice_lo  = v2 - v3;

    // Gate slice_hi by whether the mid-band has matching evidence of a bump.
    // Threshold is derived from gScale (no extra slider) — small mid-band
    // signal (≤ scale*0.5) → kill the high-freq slice (likely spike).
    float threshold = max(gScale, 1e-5) * 0.5;
    float keep      = smoothstep(threshold, threshold * 2.0, abs(slice_mid));

    // Frequency-falloff curve: low-freq slice gets full weight, mid gets
    // gHfWeight, high gets gHfWeight² (with the spike gate). Power-curve so
    // dune-scale bumps win on amplitude over rocky high-freq textures.
    float w_md = gHfWeight;
    float w_hi = gHfWeight * gHfWeight * keep;
    float bp   = (slice_lo + slice_mid * w_md + slice_hi * w_hi) / refLum;

    // Soft saturation: bounds |output| < 1 regardless of input magnitude.
    //   Small bp → roughly bp/gScale (linear, preserves subtle detail).
    //   Large bp → asymptotes to ±1 (caps spikes; equalizes across textures).
    // Smaller gScale = more aggressive equalization (subtle and bumpy textures
    // both produce displacement near ±1). Larger = more dynamic range preserved.
    float shaped = bp / (abs(bp) + max(gScale, 1e-5));

    // Bias is a pure additive offset; amp is the final magnitude.
    float h = (shaped + gDisplacementBias) * ampScaledTotal;

    // Displace along a blend of surface normal and world-up. Blend = 1 uses
    // pure world-up, identical across all chunks (no boundary normal divergence
    // → no cracks). Blend = 0 follows surface normal — correct on slopes/cliffs
    // but exposes any boundary normal mismatches as visible seams.
    float3 dispDir = normalize(lerp(normalize(o.normal), float3(0, 1, 0), gDispDirWorldUp));
    float4 dispClip = dispDir.x * patch[0].wvpCol0
                    + dispDir.y * wvpCol1
                    + dispDir.z * patch[0].wvpCol2;
    o.pos = passClip + h * dispClip;
    o.worldPos += h * dispDir;
    return o;
}
)HLSL";


// ============================================================================
// Globals
// ============================================================================

ID3D11HullShader*         gHs            = nullptr;
ID3D11DomainShader*       gDs            = nullptr;
ID3D11SamplerState*       gLinearWrap    = nullptr;
ID3D11Buffer*             gControlCb     = nullptr;
ID3D11RasterizerState*    gWireframeRs   = nullptr;
Controls                  gControls;

// Captured PS bytecode by PS pointer (kept for reflection logging on first
// sighting of each unique PS — useful when debugging cb0 layout changes).
std::unordered_map<ID3D11PixelShader*, std::vector<uint8_t>> gPsBytecode;

// Per-PS blend level: -1 = simple_fs (distant terrain, no scales — exclude),
// 0..3 = main_fs with that many BLEND# extension layers. We tessellate level
// >= 0 only.
std::unordered_map<ID3D11PixelShader*, int> gPsBlendLevel;

// Captured BLEND1/2/3 channel-index #defines (0..3 = R/G/B/A) per PS.
// OnTerrainPsCompiled stashes by bytecode hash (called from the D3DCompile
// hook). OnPixelShaderCreated re-hashes the bytecode and moves the entry into
// the PS-pointer map for per-draw lookup.
struct PsBlendDefines { int b1 = -1, b2 = -1, b3 = -1; };
static std::unordered_map<uint64_t, PsBlendDefines>            gBlendDefsByHash;
static std::unordered_map<ID3D11PixelShader*, PsBlendDefines>  gPsBlendDefs;

static uint64_t HashBytecode(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

struct SavedState
{
    D3D11_PRIMITIVE_TOPOLOGY  topo    = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11HullShader*         hs      = nullptr;
    ID3D11DomainShader*       ds      = nullptr;
    ID3D11Buffer*             hsCb0   = nullptr;
    ID3D11Buffer*             hsCb1   = nullptr;
    ID3D11Buffer*             dsCb0   = nullptr;
    ID3D11Buffer*             dsCb1   = nullptr;
    ID3D11ShaderResourceView* dsSrv[7] = { nullptr };  // overlayMap, diffuseMaps[0/1/2/3], colourMap, blendMap
    ID3D11SamplerState*       dsSamp0 = nullptr;
    ID3D11Buffer*             psCb1   = nullptr;
};
SavedState gSaved;

bool CompileShader(const char* src, const char* target, ID3DBlob** outBlob)
{
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, 0, 0, outBlob, &errors);
    if (FAILED(hr))
    {
        Log("TerrainTess: compile failed (%s): %s",
            target, errors ? (const char*)errors->GetBufferPointer() : "no error blob");
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

} // anonymous namespace


// ============================================================================
// Public API: GetControls / Enabled / GpuTimeMs
// ============================================================================

Controls* GetControls() { return &gControls; }

static bool gEnabled = true;
bool GetEnabled() { return gEnabled; }
void SetEnabled(bool enabled) { gEnabled = enabled; }


// Per-frame GPU timing. Each tess draw is bracketed with timestamp queries
// under one disjoint per frame; we read the previous frame's results to avoid
// a CPU stall. The plugin polls GetGpuTimeMs() each frame.
namespace {
struct TimerFrame
{
    ID3D11Query*              disjoint = nullptr;
    std::vector<ID3D11Query*> tsBegin;
    std::vector<ID3D11Query*> tsEnd;
    UINT                      used = 0;       // begin/end pairs issued this frame
    bool                      issued = false; // disjoint+queries End'd; safe to read
};
constexpr int kTimerFrames      = 3;     // ring depth — avoid CPU stalls
constexpr int kMaxDrawsPerFrame = 256;
TimerFrame gTimerFrames[kTimerFrames];
int        gTimerCur  = 0;
float      gGpuTimeMs = 0.0f;
} // anon

float GetGpuTimeMs() { return gGpuTimeMs; }

void OnFrameEnd()
{
    ID3D11DeviceContext* ctx = D3D11Hook::gContext;
    if (!ctx) return;

    auto& cur = gTimerFrames[gTimerCur];
    if (cur.used > 0 && cur.disjoint)
    {
        ctx->End(cur.disjoint);
        cur.issued = true;
    }

    int nextIdx = (gTimerCur + 1) % kTimerFrames;
    auto& nxt = gTimerFrames[nextIdx];

    if (nxt.issued && nxt.disjoint)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        HRESULT djHr = ctx->GetData(nxt.disjoint, &dj, sizeof(dj), 0);
        if (djHr == S_OK && !dj.Disjoint && dj.Frequency > 0)
        {
            UINT64 totalDelta = 0;
            for (UINT i = 0; i < nxt.used; i++)
            {
                UINT64 t0 = 0, t1 = 0;
                if (ctx->GetData(nxt.tsBegin[i], &t0, sizeof(t0), 0) == S_OK &&
                    ctx->GetData(nxt.tsEnd[i],   &t1, sizeof(t1), 0) == S_OK)
                {
                    totalDelta += (t1 - t0);
                }
            }
            gGpuTimeMs = (float)((double)totalDelta * 1000.0 / (double)dj.Frequency);
        }
        nxt.issued = false;
    }

    gTimerCur = nextIdx;
    gTimerFrames[gTimerCur].used = 0;
}


// ============================================================================
// PS / VS hooks for BLEND-define capture and reflection logging
// ============================================================================

void OnPixelShaderCreated(const void* bytecode, size_t size, ID3D11PixelShader* ps)
{
    if (!bytecode || !ps || size == 0) return;
    auto& vec = gPsBytecode[ps];
    vec.resize(size);
    memcpy(vec.data(), bytecode, size);

    // Classify: detect main-terrain PS by looking for "scalesA" at offset 48
    // in any cbuffer. Then determine blend level by counting normalMaps1/2/3
    // bindings.
    int blendLevel = -1;
    ID3D11ShaderReflection* refl = nullptr;
    if (SUCCEEDED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, (void**)&refl)) && refl)
    {
        D3D11_SHADER_DESC sd = {};
        refl->GetDesc(&sd);
        bool hasScalesA = false;
        for (UINT i = 0; i < sd.ConstantBuffers && !hasScalesA; i++)
        {
            ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
            D3D11_SHADER_BUFFER_DESC bd = {};
            cb->GetDesc(&bd);
            for (UINT v = 0; v < bd.Variables; v++)
            {
                ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                D3D11_SHADER_VARIABLE_DESC vd = {};
                var->GetDesc(&vd);
                if (vd.Name && strcmp(vd.Name, "scalesA") == 0 && vd.StartOffset == 48)
                {
                    hasScalesA = true;
                    break;
                }
            }
        }
        if (hasScalesA)
        {
            blendLevel = 0;
            for (UINT j = 0; j < sd.BoundResources; j++)
            {
                D3D11_SHADER_INPUT_BIND_DESC ib = {};
                refl->GetResourceBindingDesc(j, &ib);
                if (!ib.Name) continue;
                if (strcmp(ib.Name, "normalMaps3") == 0) { blendLevel = 3; break; }
                if (strcmp(ib.Name, "normalMaps2") == 0) blendLevel = (blendLevel < 2 ? 2 : blendLevel);
                if (strcmp(ib.Name, "normalMaps1") == 0) blendLevel = (blendLevel < 1 ? 1 : blendLevel);
            }
        }
        refl->Release();
    }
    gPsBlendLevel[ps] = blendLevel;

    // If a BLEND-defines record was stashed by OnTerrainPsCompiled (called
    // earlier from the D3DCompile hook), associate it with the PS pointer.
    auto bdIt = gBlendDefsByHash.find(HashBytecode(bytecode, size));
    if (bdIt != gBlendDefsByHash.end())
    {
        gPsBlendDefs[ps] = bdIt->second;
        gBlendDefsByHash.erase(bdIt);
    }
}

void OnTerrainPsCompiled(const void* bytecode, size_t size,
                         int blend1, int blend2, int blend3)
{
    if (!bytecode || size == 0) return;
    PsBlendDefines bd;
    bd.b1 = blend1; bd.b2 = blend2; bd.b3 = blend3;
    gBlendDefsByHash[HashBytecode(bytecode, size)] = bd;
}

void LogShaderSignature(const void* bytecode, size_t size, const char* tag)
{
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, (void**)&refl)) || !refl)
    {
        Log("LogShaderSignature(%s): D3DReflect failed", tag);
        return;
    }

    D3D11_SHADER_DESC desc;
    refl->GetDesc(&desc);
    Log("=== Signature %s: %u in, %u out, %u patchOut ===",
        tag, desc.InputParameters, desc.OutputParameters, desc.PatchConstantParameters);

    for (UINT i = 0; i < desc.InputParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetInputParameterDesc(i, &p);
        Log("  IN [%u] %s%u sysVal=%u mask=0x%X",
            i, p.SemanticName ? p.SemanticName : "?",
            p.SemanticIndex, (UINT)p.SystemValueType, (UINT)p.Mask);
    }
    for (UINT i = 0; i < desc.OutputParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetOutputParameterDesc(i, &p);
        Log("  OUT[%u] %s%u sysVal=%u mask=0x%X",
            i, p.SemanticName ? p.SemanticName : "?",
            p.SemanticIndex, (UINT)p.SystemValueType, (UINT)p.Mask);
    }
    for (UINT i = 0; i < desc.PatchConstantParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetPatchConstantParameterDesc(i, &p);
        Log("  PATCH[%u] %s%u sysVal=%u mask=0x%X",
            i, p.SemanticName ? p.SemanticName : "?",
            p.SemanticIndex, (UINT)p.SystemValueType, (UINT)p.Mask);
    }

    refl->Release();
}


// ============================================================================
// Init / Shutdown
// ============================================================================

void Init(ID3D11Device* device)
{
    if (!device) return;
    if (gHs && gDs) return;

    ID3DBlob* hsBlob = nullptr;
    if (!CompileShader(kPassthroughHS, "hs_5_0", &hsBlob))
        return;

    ID3DBlob* dsBlob = nullptr;
    if (!CompileShader(kPassthroughDS, "ds_5_0", &dsBlob))
    {
        hsBlob->Release();
        return;
    }

    HRESULT hr = device->CreateHullShader(hsBlob->GetBufferPointer(),
                                          hsBlob->GetBufferSize(),
                                          nullptr, &gHs);
    if (FAILED(hr))
    {
        Log("TerrainTess: CreateHullShader failed (0x%08X)", hr);
        hsBlob->Release();
        dsBlob->Release();
        return;
    }

    hr = device->CreateDomainShader(dsBlob->GetBufferPointer(),
                                    dsBlob->GetBufferSize(),
                                    nullptr, &gDs);
    if (FAILED(hr))
    {
        Log("TerrainTess: CreateDomainShader failed (0x%08X)", hr);
        gHs->Release(); gHs = nullptr;
        hsBlob->Release();
        dsBlob->Release();
        return;
    }

    LogShaderSignature(hsBlob->GetBufferPointer(), hsBlob->GetBufferSize(), "HS");
    LogShaderSignature(dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), "DS");

    hsBlob->Release();
    dsBlob->Release();

    // Linear-wrap sampler for DS texture sampling. Matches the addressing
    // mode of Kenshi's `Linear` and `Anisotropic` samplers (both wrap).
    {
        D3D11_SAMPLER_DESC ss = {};
        ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        ss.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        ss.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        ss.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&ss, &gLinearWrap);
    }

    // Dynamic cbuffer for tunable parameters (sizeof(Controls) = 96 bytes).
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(Controls);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &gControlCb);
    }

    // Per-frame timestamp pool for the GPU-time perf metric.
    {
        D3D11_QUERY_DESC qd = {};
        for (int f = 0; f < kTimerFrames; f++)
        {
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            device->CreateQuery(&qd, &gTimerFrames[f].disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            gTimerFrames[f].tsBegin.resize(kMaxDrawsPerFrame);
            gTimerFrames[f].tsEnd.resize(kMaxDrawsPerFrame);
            for (int i = 0; i < kMaxDrawsPerFrame; i++)
            {
                device->CreateQuery(&qd, &gTimerFrames[f].tsBegin[i]);
                device->CreateQuery(&qd, &gTimerFrames[f].tsEnd[i]);
            }
        }
    }

    Log("TerrainTess: HS+DS compiled and ready");
}

void Shutdown()
{
    if (gHs)          { gHs->Release();          gHs = nullptr; }
    if (gDs)          { gDs->Release();          gDs = nullptr; }
    if (gLinearWrap)  { gLinearWrap->Release();  gLinearWrap = nullptr; }
    if (gControlCb)   { gControlCb->Release();   gControlCb = nullptr; }
    if (gWireframeRs) { gWireframeRs->Release(); gWireframeRs = nullptr; }
    for (int f = 0; f < kTimerFrames; f++)
    {
        if (gTimerFrames[f].disjoint) { gTimerFrames[f].disjoint->Release(); gTimerFrames[f].disjoint = nullptr; }
        for (auto* q : gTimerFrames[f].tsBegin) if (q) q->Release();
        for (auto* q : gTimerFrames[f].tsEnd)   if (q) q->Release();
        gTimerFrames[f].tsBegin.clear();
        gTimerFrames[f].tsEnd.clear();
        gTimerFrames[f].used = 0;
        gTimerFrames[f].issued = false;
    }
    gTimerCur = 0;
    gGpuTimeMs = 0.0f;
    gPsBytecode.clear();
    gPsBlendLevel.clear();
    gPsBlendDefs.clear();
    gBlendDefsByHash.clear();
}


// ============================================================================
// Internal: filter draws + IB conversion
// ============================================================================

namespace {

bool IsTerrainShaderBound(ID3D11DeviceContext* ctx)
{
    if (!gEnabled) return false;

    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, 0);
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, 0);

    bool ok = false;
    if (vs && ps)
    {
        bool catOk = (ShaderDatabase::GetVertexShaderCategory(vs) == DUST_SHADER_TERRAIN) &&
                     (ShaderDatabase::GetPixelShaderCategory(ps) == DUST_SHADER_TERRAIN);
        // Tessellate any main_fs draw (level >= 0). simple_fs (distant
        // terrain, level=-1) has no scales and stays excluded.
        auto it = gPsBlendLevel.find(ps);
        int level = (it != gPsBlendLevel.end()) ? it->second : -1;
        ok = catOk && (level >= 0);
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    return ok;
}

// Convert a TRIANGLESTRIP IB sub-range to TRIANGLELIST with alternating winding
// + degenerate skip. Caller must Release the returned listIB after use.
//
// No caching: Kenshi reuses the same IB pointer with MAP_DISCARD writes for
// different chunks, so any cache keyed by (IB,start,count) returns stale data
// when content changes between draws (visible as fan artifacts at chunk seams
// in wireframe). Always re-convert.
bool PrepareStripConversion(ID3D11DeviceContext* ctx,
                            ID3D11Buffer* origIB, DXGI_FORMAT origFormat,
                            UINT origByteOffset,
                            UINT stripIndexCount, UINT stripStartIndex,
                            ID3D11Buffer** outListIB, UINT* outListIndexCount)
{
    if (!origIB || stripIndexCount < 3) return false;
    if (origFormat != DXGI_FORMAT_R16_UINT && origFormat != DXGI_FORMAT_R32_UINT)
        return false;

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return false;

    UINT indexSize = (origFormat == DXGI_FORMAT_R16_UINT) ? 2 : 4;

    D3D11_BUFFER_DESC srcDesc = {};
    origIB->GetDesc(&srcDesc);

    D3D11_BUFFER_DESC stagingDesc = {};
    stagingDesc.ByteWidth      = srcDesc.ByteWidth;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Buffer* staging = nullptr;
    if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, &staging)))
    {
        device->Release();
        return false;
    }
    ctx->CopyResource(staging, origIB);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        staging->Release();
        device->Release();
        return false;
    }

    UINT triCount = stripIndexCount - 2;
    UINT maxListIndices = triCount * 3;
    void* listData = malloc((size_t)maxListIndices * indexSize);

    const uint8_t* srcBase = (const uint8_t*)mapped.pData
                           + origByteOffset
                           + (size_t)stripStartIndex * indexSize;

    // Strip → list. Even triangles emit (v0,v1,v2); odd emit (v1,v0,v2) so the
    // rasterizer sees consistent winding. Strips also use degenerate triangles
    // (two coincident vertices) as row terminators — invisible in raster but
    // tessellation breaks the degeneracy and produces long thin spikes. Skip
    // any triangle where two indices match.
    UINT dstTri = 0;
    if (indexSize == 2)
    {
        const uint16_t* src = (const uint16_t*)srcBase;
        uint16_t* dst = (uint16_t*)listData;
        for (UINT i = 0; i < triCount; i++)
        {
            uint16_t a = src[i], b = src[i+1], c = src[i+2];
            if (a == b || b == c || a == c) continue;
            if (i & 1) { dst[dstTri*3+0] = b; dst[dstTri*3+1] = a; dst[dstTri*3+2] = c; }
            else       { dst[dstTri*3+0] = a; dst[dstTri*3+1] = b; dst[dstTri*3+2] = c; }
            dstTri++;
        }
    }
    else
    {
        const uint32_t* src = (const uint32_t*)srcBase;
        uint32_t* dst = (uint32_t*)listData;
        for (UINT i = 0; i < triCount; i++)
        {
            uint32_t a = src[i], b = src[i+1], c = src[i+2];
            if (a == b || b == c || a == c) continue;
            if (i & 1) { dst[dstTri*3+0] = b; dst[dstTri*3+1] = a; dst[dstTri*3+2] = c; }
            else       { dst[dstTri*3+0] = a; dst[dstTri*3+1] = b; dst[dstTri*3+2] = c; }
            dstTri++;
        }
    }
    UINT listIndexCount = dstTri * 3;

    ctx->Unmap(staging, 0);
    staging->Release();

    D3D11_BUFFER_DESC newDesc = {};
    newDesc.ByteWidth = listIndexCount * indexSize;
    newDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    newDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = listData;

    ID3D11Buffer* listIB = nullptr;
    HRESULT hr = device->CreateBuffer(&newDesc, &initData, &listIB);
    free(listData);
    device->Release();

    if (FAILED(hr)) return false;

    *outListIB         = listIB;
    *outListIndexCount = listIndexCount;
    return true;
}

} // anonymous namespace


// ============================================================================
// Begin / End — set up and tear down DS pipeline state per draw
// ============================================================================

void Begin(ID3D11DeviceContext* ctx)
{
    // Save state Kenshi/Ogre relies on (HS/DS, cbuffers, SRVs, samplers) so
    // we can restore it after our draw.
    ctx->IAGetPrimitiveTopology(&gSaved.topo);
    ctx->HSGetShader(&gSaved.hs, nullptr, 0);
    ctx->DSGetShader(&gSaved.ds, nullptr, 0);
    ctx->HSGetConstantBuffers(0, 1, &gSaved.hsCb0);
    ctx->HSGetConstantBuffers(1, 1, &gSaved.hsCb1);
    ctx->DSGetConstantBuffers(0, 1, &gSaved.dsCb0);
    ctx->DSGetConstantBuffers(1, 1, &gSaved.dsCb1);
    ctx->DSGetShaderResources(0, 7, gSaved.dsSrv);
    ctx->DSGetSamplers(0, 1, &gSaved.dsSamp0);
    ctx->PSGetConstantBuffers(1, 1, &gSaved.psCb1);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ctx->HSSetShader(gHs, nullptr, 0);
    ctx->DSSetShader(gDs, nullptr, 0);

    // Defensive: clear any GS that might be bound between DS and rasterizer.
    ID3D11GeometryShader* nullGs = nullptr;
    ctx->GSSetShader(nullGs, nullptr, 0);

    // Mirror PS cb0 (terrain $Globals: scalesA/B/C, slopeMin/Max/Blend, etc.)
    // to DS cb0 so the DS replica reads the same per-chunk uniforms.
    {
        ID3D11Buffer* psCb0 = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &psCb0);
        ctx->DSSetConstantBuffers(0, 1, &psCb0);

        // Diagnostic: reflect every unique PS that hits this code path so we
        // can verify cb0 layout matches our DS expectations. One-shot per PS.
        static std::unordered_map<ID3D11PixelShader*, bool> sLoggedPs;
        ID3D11PixelShader* curPs = nullptr;
        ctx->PSGetShader(&curPs, nullptr, 0);
        if (curPs && sLoggedPs.find(curPs) == sLoggedPs.end())
        {
            sLoggedPs[curPs] = true;
            auto bcIt = gPsBytecode.find(curPs);
            if (bcIt != gPsBytecode.end())
            {
                Log("TerrainTess: reflecting bound PS=%p bytecodeSize=%zu",
                    curPs, bcIt->second.size());
                ID3D11ShaderReflection* refl = nullptr;
                if (SUCCEEDED(D3DReflect(bcIt->second.data(), bcIt->second.size(),
                                          IID_ID3D11ShaderReflection, (void**)&refl))
                    && refl)
                {
                    D3D11_SHADER_DESC sd = {};
                    refl->GetDesc(&sd);
                    Log("  ConstantBuffers=%u  BoundResources=%u",
                        sd.ConstantBuffers, sd.BoundResources);
                    for (UINT j = 0; j < sd.BoundResources; j++)
                    {
                        D3D11_SHADER_INPUT_BIND_DESC ib = {};
                        refl->GetResourceBindingDesc(j, &ib);
                        const char* tname = "?";
                        switch (ib.Type) {
                            case D3D_SIT_CBUFFER: tname = "CBUFFER"; break;
                            case D3D_SIT_TEXTURE: tname = "TEXTURE"; break;
                            case D3D_SIT_SAMPLER: tname = "SAMPLER"; break;
                            default: break;
                        }
                        Log("    bind '%s' type=%s slot=%u dim=%u",
                            ib.Name, tname, ib.BindPoint, (UINT)ib.Dimension);
                    }
                    for (UINT i = 0; i < sd.ConstantBuffers; i++)
                    {
                        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
                        D3D11_SHADER_BUFFER_DESC bd = {};
                        cb->GetDesc(&bd);
                        Log("  cb%u '%s' size=%u vars=%u", i, bd.Name, bd.Size, bd.Variables);
                        for (UINT v = 0; v < bd.Variables; v++)
                        {
                            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                            D3D11_SHADER_VARIABLE_DESC vd = {};
                            var->GetDesc(&vd);
                            Log("    %-30s offset=%4u size=%4u", vd.Name, vd.StartOffset, vd.Size);
                        }
                    }
                    refl->Release();
                }
            }
        }
        if (curPs) curPs->Release();

        if (psCb0) psCb0->Release();
    }

    // Mirror PS textures to DS slots 0..6:
    //   t0 overlayMap   ← PS slot 2
    //   t1 diffuseMaps  ← PS slot 0
    //   t2 colourMap    ← PS slot 3
    //   t3 diffuseMaps1 ← PS slot 6
    //   t4 diffuseMaps2 ← PS slot 8
    //   t5 diffuseMaps3 ← PS slot 10
    //   t6 blendMap     ← PS slot 5
    {
        ID3D11ShaderResourceView* psSrvs[7] = {};
        ctx->PSGetShaderResources(2,  1, &psSrvs[0]);
        ctx->PSGetShaderResources(0,  1, &psSrvs[1]);
        ctx->PSGetShaderResources(3,  1, &psSrvs[2]);
        ctx->PSGetShaderResources(6,  1, &psSrvs[3]);
        ctx->PSGetShaderResources(8,  1, &psSrvs[4]);
        ctx->PSGetShaderResources(10, 1, &psSrvs[5]);
        ctx->PSGetShaderResources(5,  1, &psSrvs[6]);
        ctx->DSSetShaderResources(0, 7, psSrvs);
        for (int i = 0; i < 7; i++) if (psSrvs[i]) psSrvs[i]->Release();
    }
    ctx->DSSetSamplers(0, 1, &gLinearWrap);

    // Populate per-PS BLEND# channel masks so the DS replica weights the
    // BLEND1/2/3 layers exactly as the PS does. Critical: every patched
    // terrain PS must have its #defines captured (see ShaderPatch's
    // captureBlendDefines lambda) — without this, masks stay zero, the DS
    // computes BLEND0-only and visible mesh seams reappear at multi-layer
    // chunk boundaries.
    for (int i = 0; i < 4; i++)
    {
        gControls.blend1Mask[i] = 0.0f;
        gControls.blend2Mask[i] = 0.0f;
        gControls.blend3Mask[i] = 0.0f;
    }
    {
        ID3D11PixelShader* curPs = nullptr;
        ctx->PSGetShader(&curPs, nullptr, 0);
        if (curPs)
        {
            auto it = gPsBlendDefs.find(curPs);
            if (it != gPsBlendDefs.end())
            {
                if (it->second.b1 >= 0 && it->second.b1 < 4) gControls.blend1Mask[it->second.b1] = 1.0f;
                if (it->second.b2 >= 0 && it->second.b2 < 4) gControls.blend2Mask[it->second.b2] = 1.0f;
                if (it->second.b3 >= 0 && it->second.b3 < 4) gControls.blend3Mask[it->second.b3] = 1.0f;
            }
            curPs->Release();
        }
    }

    // Upload TessControl cbuffer with current mask + plugin values.
    if (gControlCb)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(gControlCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, &gControls, sizeof(gControls));
            ctx->Unmap(gControlCb, 0);
        }
        ctx->HSSetConstantBuffers(1, 1, &gControlCb);
        ctx->DSSetConstantBuffers(1, 1, &gControlCb);
        // PS reads gDebugViewMode for the debug-view patch.
        ctx->PSSetConstantBuffers(1, 1, &gControlCb);
    }
}

void End(ID3D11DeviceContext* ctx)
{
    ctx->IASetPrimitiveTopology(gSaved.topo);
    ctx->HSSetShader(gSaved.hs, nullptr, 0);
    ctx->DSSetShader(gSaved.ds, nullptr, 0);
    ctx->HSSetConstantBuffers(0, 1, &gSaved.hsCb0);
    ctx->HSSetConstantBuffers(1, 1, &gSaved.hsCb1);
    ctx->DSSetConstantBuffers(0, 1, &gSaved.dsCb0);
    ctx->DSSetConstantBuffers(1, 1, &gSaved.dsCb1);
    ctx->DSSetShaderResources(0, 7, gSaved.dsSrv);
    ctx->DSSetSamplers(0, 1, &gSaved.dsSamp0);
    ctx->PSSetConstantBuffers(1, 1, &gSaved.psCb1);

    if (gSaved.hs)      { gSaved.hs->Release();      gSaved.hs = nullptr; }
    if (gSaved.ds)      { gSaved.ds->Release();      gSaved.ds = nullptr; }
    if (gSaved.hsCb0)   { gSaved.hsCb0->Release();   gSaved.hsCb0 = nullptr; }
    if (gSaved.hsCb1)   { gSaved.hsCb1->Release();   gSaved.hsCb1 = nullptr; }
    if (gSaved.dsCb0)   { gSaved.dsCb0->Release();   gSaved.dsCb0 = nullptr; }
    if (gSaved.dsCb1)   { gSaved.dsCb1->Release();   gSaved.dsCb1 = nullptr; }
    for (int i = 0; i < 7; i++) if (gSaved.dsSrv[i]) { gSaved.dsSrv[i]->Release(); gSaved.dsSrv[i] = nullptr; }
    if (gSaved.dsSamp0) { gSaved.dsSamp0->Release(); gSaved.dsSamp0 = nullptr; }
    if (gSaved.psCb1)   { gSaved.psCb1->Release();   gSaved.psCb1 = nullptr; }
    gSaved.topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}


// ============================================================================
// TryDrawTessellated — main entry from the DrawIndexed hook
// ============================================================================

namespace {

bool TimerBeginDraw(ID3D11DeviceContext* ctx)
{
    auto& tf = gTimerFrames[gTimerCur];
    if (!tf.disjoint || tf.used >= tf.tsBegin.size()) return false;
    if (tf.used == 0) ctx->Begin(tf.disjoint);
    ctx->End(tf.tsBegin[tf.used]);
    return true;
}
void TimerEndDraw(ID3D11DeviceContext* ctx, bool started)
{
    if (!started) return;
    auto& tf = gTimerFrames[gTimerCur];
    ctx->End(tf.tsEnd[tf.used]);
    tf.used++;
}

} // anon

bool TryDrawTessellated(ID3D11DeviceContext* ctx,
                        UINT indexCount, UINT startIndex,
                        INT baseVertex, DrawIndexedFn drawFn)
{
    if (!ctx || !gHs || !gDs || !drawFn) return false;
    if (!IsTerrainShaderBound(ctx)) return false;

    // Lazy-create wireframe rasterizer state on first wireframe-mode draw.
    if (gControls.wireframe > 0.5f && !gWireframeRs)
    {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev)
        {
            D3D11_RASTERIZER_DESC rd = {};
            rd.FillMode = D3D11_FILL_WIREFRAME;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            dev->CreateRasterizerState(&rd, &gWireframeRs);
            dev->Release();
        }
    }

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);

    // Mode 2: vanilla strip with wireframe rasterizer (no tess, no IB conversion).
    if (gControls.wireframe > 1.5f && gControls.wireframe < 2.5f)
    {
        ID3D11RasterizerState* prevRs = nullptr;
        ctx->RSGetState(&prevRs);
        if (gWireframeRs) ctx->RSSetState(gWireframeRs);
        drawFn(ctx, indexCount, startIndex, baseVertex);
        ctx->RSSetState(prevRs);
        if (prevRs) prevRs->Release();
        return true;
    }

    // Mode 3: strip→list IB conversion + TRIANGLELIST, NO HS/DS. Isolates the
    // IB conversion from the tess pipeline so we can verify the converted
    // triangles match what strip rasterization would produce.
    if (gControls.wireframe > 2.5f && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    {
        ID3D11Buffer* origIB = nullptr;
        DXGI_FORMAT   origFormat = DXGI_FORMAT_UNKNOWN;
        UINT          origOffset = 0;
        ctx->IAGetIndexBuffer(&origIB, &origFormat, &origOffset);
        if (!origIB) return false;
        ID3D11Buffer* listIB = nullptr;
        UINT listIC = 0;
        bool ok = PrepareStripConversion(ctx, origIB, origFormat, origOffset,
                                         indexCount, startIndex, &listIB, &listIC);
        if (!ok) { origIB->Release(); return false; }
        ID3D11RasterizerState* prevRs = nullptr;
        ctx->RSGetState(&prevRs);
        ID3D11HullShader*   prevHs = nullptr; ctx->HSGetShader(&prevHs, nullptr, nullptr);
        ID3D11DomainShader* prevDs = nullptr; ctx->DSGetShader(&prevDs, nullptr, nullptr);
        if (gWireframeRs) ctx->RSSetState(gWireframeRs);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetIndexBuffer(listIB, origFormat, 0);
        drawFn(ctx, listIC, 0, baseVertex);
        ctx->IASetIndexBuffer(origIB, origFormat, origOffset);
        ctx->IASetPrimitiveTopology(topo);
        ctx->HSSetShader(prevHs, nullptr, 0);
        ctx->DSSetShader(prevDs, nullptr, 0);
        ctx->RSSetState(prevRs);
        if (prevHs) prevHs->Release();
        if (prevDs) prevDs->Release();
        if (prevRs) prevRs->Release();
        if (listIB) listIB->Release();
        origIB->Release();
        return true;
    }

    // Mode 1 = tess wireframe: same tess path with wireframe rasterizer.
    bool wfTess = (gControls.wireframe > 0.5f && gControls.wireframe < 1.5f);

    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
    {
        bool timed = TimerBeginDraw(ctx);
        Begin(ctx);
        if (wfTess && gWireframeRs) ctx->RSSetState(gWireframeRs);
        drawFn(ctx, indexCount, startIndex, baseVertex);
        End(ctx);
        TimerEndDraw(ctx, timed);
        return true;
    }

    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    {
        ID3D11Buffer* origIB = nullptr;
        DXGI_FORMAT   origFormat = DXGI_FORMAT_UNKNOWN;
        UINT          origOffset = 0;
        ctx->IAGetIndexBuffer(&origIB, &origFormat, &origOffset);
        if (!origIB) return false;

        ID3D11Buffer* listIB = nullptr;
        UINT          listIC = 0;
        if (!PrepareStripConversion(ctx, origIB, origFormat, origOffset,
                                    indexCount, startIndex, &listIB, &listIC))
        {
            origIB->Release();
            return false;
        }

        ctx->IASetIndexBuffer(listIB, origFormat, 0);
        bool timed = TimerBeginDraw(ctx);
        Begin(ctx);
        if (wfTess && gWireframeRs) ctx->RSSetState(gWireframeRs);
        drawFn(ctx, listIC, 0, baseVertex);
        End(ctx);
        TimerEndDraw(ctx, timed);
        ctx->IASetIndexBuffer(origIB, origFormat, origOffset);
        if (listIB) listIB->Release();
        origIB->Release();
        return true;
    }

    return false;
}

} // namespace TerrainTess
