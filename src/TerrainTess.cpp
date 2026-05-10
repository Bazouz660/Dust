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
#include <complex>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TerrainTess
{

namespace {

// VS output layout for terrain.hlsl main_vs (TEXTURED variant — used in both
// terrainfp4 GBuffer paths). HS/DS structs must match this layout exactly.
//   POSITION  : float4 (clip-space position from worldViewProjMatrix)
//   TEXCOORD0 : float3 (world-space normal)
//   TEXCOORD1 : float3 (world-space position, for distance/parallax)
//   TEXCOORD2 : float3 (terrain triplanar/horizontal UV)
//   TEXCOORD3 : float4 (overlay & biome map UVs)
//   TEXCOORD4 : float2 (cliff blend weights)
//   TEXCOORD5 : float4 (vertical-cliff distortion offsets)
//
// The shadow VS variant has only POSITION + a single TEXCOORD0 (depth) and
// is NOT handled by the current spike — IsTerrainDraw filters it out by
// requiring the PS to also classify as TERRAIN.
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
    float gUvTileFactor;
    float gDebugSlice;
    float gDebugViewMode;
    float gDisplacementBias;
    float gFactorSnapStep;
    float gDispDirWorldUp;
    float gMipBias;
    float gWireframeMode;
    float _ctrlPad_;
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
    // hop tess vertices in and out. Adjacent patches that share an edge
    // see the same wEdge values and snap to the same factor → no T-junctions.
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

Texture2DArray heightArr  : register(t0);
Texture2D      overlayMap : register(t1);
// Cross-region neighbor heightArrays — kept for future neighbor-aware blends
// but currently unused; default-bound to primary heightArr.
Texture2DArray heightArrNX : register(t2);
Texture2DArray heightArrPX : register(t3);
Texture2DArray heightArrNZ : register(t4);
Texture2DArray heightArrPZ : register(t5);
// Live PS textures mirrored per draw — the DS replicates the PS BLEND chain
// over these to compute displacement = PS-visible luminance, eliminating
// seams (since both sides of any chunk boundary compute the same lum the PS
// computes, which is texture-continuous by art-side construction).
Texture2DArray diffuseMaps  : register(t6);
Texture2D      colourMap    : register(t7);
Texture2DArray diffuseMaps1 : register(t8);
Texture2DArray diffuseMaps2 : register(t9);
Texture2DArray diffuseMaps3 : register(t10);
Texture2D      blendMap     : register(t11);
SamplerState   heightSamp   : register(s0);    // host binds Anisotropic here

// Mirror of PS $Globals cbuffer. Offsets verified by reflection at runtime
// (see Dust.log after first terrain draw): each BLEND# layer occupies
// 160 bytes (10 × float4) starting at 48 / 224 / 384 / 544.
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
    float4 _padBlend1       : packoffset(c21);
    float4 _padBlend1b      : packoffset(c22);
    float4 _padBlend1c      : packoffset(c23);
    // BLEND2 (offsets 384..543).
    float4 gPsScalesA2      : packoffset(c24);
    float4 gPsScalesB2      : packoffset(c25);
    float4 gPsScalesC2      : packoffset(c26);
    float4 gPsSlopeMin2     : packoffset(c27);
    float4 gPsSlopeMax2     : packoffset(c28);
    float4 gPsSlopeBlend2   : packoffset(c29);
    float4 gPsOverlayMult2  : packoffset(c30);
    float4 _padBlend2       : packoffset(c31);
    float4 _padBlend2b      : packoffset(c32);
    float4 _padBlend2c      : packoffset(c33);
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
    float gUvTileFactor;
    float gDebugSlice;
    float gDebugViewMode;       // 0=off, 1=PS lum overlay, 2=DS chunk-edge ridge debug
    float gDisplacementBias;
    float gFactorSnapStep;
    float gDispDirWorldUp;
    float gMipBias;
    float gWireframeMode;
    float gChunkSizeWorld;
    // Host-set per draw.
    float gChunkOriginX;
    float gChunkOriginZ;
    float2 _ctrlPadTail;
    // Per-PS blendMap channel selectors: PS variants use BLEND1/2/3 #defines
    // to pick which channel of blendMap weights each layer. Host fills these
    // per-draw from captured PS defines. (1,0,0,0)=R, (0,1,0,0)=G, etc.
    // All-zero = layer inactive; the BLEND chain skips it.
    float4 gBlend1Mask;
    float4 gBlend2Mask;
    float4 gBlend3Mask;
};

float Lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// One BLEND layer's albedo, mirroring computeBiome from terrainfp4.hlsl.
// Skips distance-fadeout (we don't have distance.a/distant.rgb) and the
// makeWet wetness modulation — both irrelevant for displacement amplitude.
float3 ComputeBiomeAlbedo(Texture2DArray dmap, float3 tc, float2 cliffBlend,
                          float4 weights, float4 omap, float3 colour,
                          float4 sA, float4 sB, float4 sC, float4 oMult)
{
    const float3 white = float3(1, 1, 1);
    float3 cB  = dmap.SampleLevel(heightSamp, float3(tc.xy * sB.xy, 0.0), 0.0).rgb * colour;
    float3 cS  = dmap.SampleLevel(heightSamp, float3(tc.xy * sA.xy, 1.0), 0.0).rgb * colour;
    float3 cG  = dmap.SampleLevel(heightSamp, float3(tc.xy * sB.zw, 3.0), 0.0).rgb * lerp(white, colour, oMult.y);
    float3 cD  = dmap.SampleLevel(heightSamp, float3(tc.xy * sC.xy, 4.0), 0.0).rgb * lerp(white, colour, oMult.z);
    float3 cR  = dmap.SampleLevel(heightSamp, float3(tc.xy * sC.zw, 5.0), 0.0).rgb * lerp(white, colour, oMult.w);
    float3 cCx = dmap.SampleLevel(heightSamp, float3(tc.yz * sA.zw, 2.0), 0.0).rgb;
    float3 cCz = dmap.SampleLevel(heightSamp, float3(tc.xz * sA.zw, 2.0), 0.0).rgb;
    float3 cC  = (cCx * cliffBlend.x + cCz * cliffBlend.y) * lerp(white, colour, oMult.x);
    float3 a = lerp(cB, cG, omap.r);
    a = lerp(a, cS, weights.x);
    a = lerp(a, cD, omap.b);
    a = lerp(a, cR, omap.a);
    a = lerp(a, cC, weights.y);
    return a;
}

// Compute the PS-visible luminance via the SAME formula the PS uses (BLEND0
// + BLEND1/2/3 chain with channel-masked blendMap weights). For chunks
// where the visible texture is continuous across a boundary (verified via
// PS debug mode 3 = pixel-exact match), this DS computation produces the
// same value on both sides of the boundary → displacement is continuous → no
// mesh seam.
float ComputePsLum(float3 tex0, float2 cliffBlend, float4 mapCoords,
                   float3 normal, float3 texV)
{
    float slope = 1.0 - normalize(normal).y;
    float4 omap = overlayMap.SampleLevel(heightSamp, mapCoords.xy, 0).rgba;
    omap.r = max(omap.r, omap.g);
    float3 colour = colourMap.SampleLevel(heightSamp, mapCoords.xy, 0).rgb * 1.2;

    float4 w0 = smoothstep(gPsSlopeMin - gPsSlopeBlend, gPsSlopeMin, slope)
              * smoothstep(gPsSlopeMax + gPsSlopeBlend, gPsSlopeMax, slope);
    float3 a = ComputeBiomeAlbedo(diffuseMaps, tex0, cliffBlend, w0, omap, colour,
                                  gPsScalesA, gPsScalesB, gPsScalesC, gPsOverlayMult);
    a *= gPsBrightnessFix.x;

    // BLEND1/2/3 chain: gated by the host-supplied channel masks.
    float4 bw = blendMap.SampleLevel(heightSamp, mapCoords.zw, 0);
    float w1 = dot(bw, gBlend1Mask);
    float w2 = dot(bw, gBlend2Mask);
    float w3 = dot(bw, gBlend3Mask);
    float wOwn = 1.0 - w1 - w2 - w3;
    a *= wOwn;

    if (w1 > 1e-5)
    {
        float3 tc1 = float3(tex0.xy, texV.x);
        float4 w1w = smoothstep(gPsSlopeMin1 - gPsSlopeBlend1, gPsSlopeMin1, slope)
                   * smoothstep(gPsSlopeMax1 + gPsSlopeBlend1, gPsSlopeMax1, slope);
        float3 a1 = ComputeBiomeAlbedo(diffuseMaps1, tc1, cliffBlend, w1w, omap, colour,
                                       gPsScalesA1, gPsScalesB1, gPsScalesC1, gPsOverlayMult1);
        a += a1 * gPsBrightnessFix.y * w1;
    }
    if (w2 > 1e-5)
    {
        float3 tc2 = float3(tex0.xy, texV.y);
        float4 w2w = smoothstep(gPsSlopeMin2 - gPsSlopeBlend2, gPsSlopeMin2, slope)
                   * smoothstep(gPsSlopeMax2 + gPsSlopeBlend2, gPsSlopeMax2, slope);
        float3 a2 = ComputeBiomeAlbedo(diffuseMaps2, tc2, cliffBlend, w2w, omap, colour,
                                       gPsScalesA2, gPsScalesB2, gPsScalesC2, gPsOverlayMult2);
        a += a2 * gPsBrightnessFix.z * w2;
    }
    if (w3 > 1e-5)
    {
        float3 tc3 = float3(tex0.xy, texV.z);
        float4 w3w = smoothstep(gPsSlopeMin3 - gPsSlopeBlend3, gPsSlopeMin3, slope)
                   * smoothstep(gPsSlopeMax3 + gPsSlopeBlend3, gPsSlopeMax3, slope);
        float3 a3 = ComputeBiomeAlbedo(diffuseMaps3, tc3, cliffBlend, w3w, omap, colour,
                                       gPsScalesA3, gPsScalesB3, gPsScalesC3, gPsOverlayMult3);
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

    // Early-out for zero amplitude (avoids the 7-slice * 5-chunk = 35 sample fetch).
    float ampScaledTotal = gAmplitude * ampScale;
    if (ampScaledTotal < 1e-4)
    {
        o.pos      = passClip;
        return o;
    }

    // Displacement = PS visible luminance computed via the exact PS BLEND
    // chain. Verified pixel-exact against the actual PS via debug mode 3
    // (terrain rendered as |gtLum - replicaLum| = ~0). Both sides of any
    // chunk boundary compute the same lum (the PS does, by construction) →
    // no displacement seam.
    float hBlend = ComputePsLum(o.tex0, o.uvblend, o.tex1, o.normal, o.texV.xyz);
    float h = (hBlend - gDisplacementBias) * ampScaledTotal;

    // Displace along a blend of surface normal and world-up. Blend = 1 uses
    // pure world-up, which is identical across all chunks (no per-vertex
    // normal divergence at chunk boundaries → no cracks). Blend = 0 follows
    // the surface normal, which gives correct displacement on slopes/cliffs
    // but reveals chunk-boundary normal mismatches as visible seams.
    float3 dispDir = normalize(lerp(normalize(o.normal), float3(0, 1, 0), gDispDirWorldUp));
    float4 dispClip = dispDir.x * patch[0].wvpCol0
                    + dispDir.y * wvpCol1
                    + dispDir.z * patch[0].wvpCol2;
    o.pos = passClip + h * dispClip;
    o.worldPos += h * dispDir;
    return o;
}
)HLSL";

// Decode pass: sample the BC3-compressed normalMaps array (which the GPU
// natively decompresses on read) and write the RGBA8 result to a UAV-backed
// texture. CPU reads back the staging copy and runs FFT-based integration.
static const char* kDecodeNormalsCS = R"HLSL(
Texture2DArray<float4>            SrcNormals : register(t0);
RWTexture2DArray<unorm float4>    DstRGBA    : register(u0);
SamplerState                      Smp        : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h, slices;
    SrcNormals.GetDimensions(w, h, slices);
    if (id.x >= w || id.y >= h) return;

    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    DstRGBA[uint3(id.xy, id.z)] = SrcNormals.SampleLevel(Smp, float3(uv, (float)id.z), 0);
}
)HLSL";

// GPU heightmap bake pipeline. Replaces the CPU FFT with five compute
// shaders that run end-to-end on the GPU:
//
//   Extract        — RGBA8 normal slice → (p, q) gradients (real, imag=0).
//   FFT_1D         — Cooley-Tukey radix-2 FFT in groupshared (axis + dir
//                    flags). One dispatch per axis, in-place butterflies.
//   Integrate      — Frankot-Chellappa quotient + Gaussian high-pass +
//                    explicit zero of the DC bin.
//   ReduceMaxAbs   — group-shared reduction → InterlockedMax on a single
//                    R32_UINT, exploiting that asuint(|float|) is monotonic.
//   Normalize      — 0.5 + H/(2*max) → R16_FLOAT array slice. R16_FLOAT
//                    is required for typed UAV writes by the D3D11.0 spec
//                    (R16_UNORM is optional and silently fails on some HW).
//
// Hardcoded N=2048 for the FFT's groupshared buffer. Smaller normal maps
// fall back to the CPU path.
static const char* kHeightBakeCS = R"HLSL(
#define N 2048
#define LOG2_N 11
#define HALF_N 1024
#define PI 3.14159265358979323846

// ============= Extract gradients =============
Texture2DArray<float4>  gNormals : register(t0);
RWTexture2D<float2>     gPout    : register(u0);
RWTexture2D<float2>     gQout    : register(u1);

cbuffer ExtractParams : register(b0)
{
    uint  eWidth, eHeight, eSlice, _epad;
};

[numthreads(8, 8, 1)]
void Extract(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= eWidth || dtid.y >= eHeight) return;
    float4 n = gNormals.Load(int4(dtid.xy, eSlice, 0));
    float nx = n.x * 2.0 - 1.0;
    float ny = n.y * 2.0 - 1.0;
    float kk = max(1.0 - nx*nx - ny*ny, 0.0025);
    float nz = sqrt(kk);
    gPout[dtid.xy] = float2(-nx / nz, 0.0);
    gQout[dtid.xy] = float2(-ny / nz, 0.0);
}

// ============= 1D FFT (groupshared, in-place after bit-reversal) =============
groupshared float2 fftLine[N];

cbuffer FftParams : register(b0)
{
    uint  fAxis;     // 0 = process rows (gid.x = row idx)
                     // 1 = process cols (gid.x = col idx)
    uint  fInverse;  // 0 = forward, 1 = inverse
    uint  _fpad0, _fpad1;
};

Texture2D<float2>     fInput  : register(t0);
RWTexture2D<float2>   fOutput : register(u0);

uint bitrev11(uint x)
{
    // Standard 32-bit bit-reverse, then keep the high 11 bits (now in
    // reversed order) by shifting right by 21.
    x = (x << 16) | (x >> 16);
    x = ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x & 0xF0F0F0F0u) >> 4);
    x = ((x & 0x33333333u) << 2) | ((x & 0xCCCCCCCCu) >> 2);
    x = ((x & 0x55555555u) << 1) | ((x & 0xAAAAAAAAu) >> 1);
    return x >> (32 - LOG2_N);
}

float2 cmul(float2 a, float2 b)
{
    return float2(a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x);
}

[numthreads(HALF_N, 1, 1)]
void FFT_1D(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    uint t = gtid.x;
    uint lineIdx = gid.x;

    int2 i0 = (fAxis == 0) ? int2(t,        lineIdx) : int2(lineIdx, t);
    int2 i1 = (fAxis == 0) ? int2(t+HALF_N, lineIdx) : int2(lineIdx, t+HALF_N);

    fftLine[bitrev11(t)]        = fInput[i0];
    fftLine[bitrev11(t+HALF_N)] = fInput[i1];
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint s = 0; s < LOG2_N; s++)
    {
        uint half_m    = 1u << s;
        uint k_in_half = t & (half_m - 1);
        uint i = ((t >> s) << (s + 1)) | k_in_half;
        uint j = i + half_m;

        float sign  = (fInverse != 0) ? 1.0 : -1.0;
        float angle = sign * 2.0 * PI * float(k_in_half) / float(half_m * 2);
        float2 tw   = float2(cos(angle), sin(angle));

        float2 a    = fftLine[i];
        float2 b_tw = cmul(fftLine[j], tw);

        fftLine[i] = a + b_tw;
        fftLine[j] = a - b_tw;
        GroupMemoryBarrierWithGroupSync();
    }

    int2 o0 = (fAxis == 0) ? int2(t,        lineIdx) : int2(lineIdx, t);
    int2 o1 = (fAxis == 0) ? int2(t+HALF_N, lineIdx) : int2(lineIdx, t+HALF_N);
    // Apply 1/N scale on inverse so a forward+inverse round-trip is identity.
    // Each 1D pass contributes 1/N; for 2D total scale is 1/N². Lets the
    // normalize step compare apples-to-apples regardless of how many FFT
    // passes the value went through (e.g. integrate/IFFT vs re-FFT/HP/IFFT).
    float scale = (fInverse != 0) ? (1.0 / float(N)) : 1.0;
    fOutput[o0] = fftLine[t]        * scale;
    fOutput[o1] = fftLine[t+HALF_N] * scale;
}

// ============= Frankot-Chellappa integrate + Gaussian high-pass =============
Texture2D<float2>    iFp : register(t0);
Texture2D<float2>    iFq : register(t1);
RWTexture2D<float2>  iH  : register(u0);

cbuffer IntegrateParams : register(b0)
{
    uint  iWidth, iHeight;
    float iHpCutoff;    // sigma in cycles/image (0 = disabled)
    float iHpStrength;  // 0..1
};

[numthreads(8, 8, 1)]
void Integrate(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= iWidth || dtid.y >= iHeight) return;

    // Signed frequency coords (matches numpy fftfreq convention).
    float u = (dtid.x < iWidth/2)  ? float(dtid.x) : (float(dtid.x) - float(iWidth));
    float v = (dtid.y < iHeight/2) ? float(dtid.y) : (float(dtid.y) - float(iHeight));
    float denom = u*u + v*v;

    float2 res = float2(0, 0);
    if (denom > 1e-12)
    {
        float2 fp = iFp[dtid.xy];
        float2 fq = iFq[dtid.xy];
        // -i * (u*fp + v*fq) / denom; -i * (a + ib) = (b, -a)
        float2 z = u*fp + v*fq;
        res = float2(z.y, -z.x) / denom;
    }

    if (iHpStrength > 0.0 && iHpCutoff > 0.0)
    {
        float atten = 1.0 - iHpStrength * exp(-(u*u + v*v) / (2.0 * iHpCutoff * iHpCutoff));
        res *= atten;
    }
    iH[dtid.xy] = res;
}

// ============= Reduce max(|H|) =============
// Trick: for non-negative floats, asuint() of the bit pattern is monotonic
// in the float value, so InterlockedMax on uint gives correct float-max.
Texture2D<float2>             rH      : register(t0);
RWStructuredBuffer<uint>      rMaxBuf : register(u0);

cbuffer ReduceParams : register(b0)
{
    uint  rWidth, rHeight, _rpad0, _rpad1;
};

groupshared uint groupMax;

[numthreads(1024, 1, 1)]
void ReduceMaxAbs(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    if (gtid.x == 0) groupMax = 0;
    GroupMemoryBarrierWithGroupSync();

    uint baseIdx = gid.x * 4096u + gtid.x * 4u;
    uint localMax = 0;
    [unroll]
    for (uint kk = 0; kk < 4u; kk++)
    {
        uint idx = baseIdx + kk;
        if (idx < rWidth * rHeight)
        {
            uint x = idx % rWidth;
            uint y = idx / rWidth;
            float val = rH[uint2(x, y)].x;
            uint asAbs = asuint(abs(val));
            localMax = max(localMax, asAbs);
        }
    }
    InterlockedMax(groupMax, localMax);
    GroupMemoryBarrierWithGroupSync();
    if (gtid.x == 0)
        InterlockedMax(rMaxBuf[0], groupMax);
}

// ============= Extract centered luminance from decoded diffuse =============
// Same FFT-bake working buffer reused: writes (lum - 0.5) into the .x of a
// float2 scratch texture. The .y is unused. Then the same ReduceMaxAbs
// shader gives us max(|lum - 0.5|) for normalization.
Texture2DArray<float4>  gDiffuse : register(t0);
RWTexture2D<float2>     gLumOut  : register(u0);

cbuffer ExtractLumParams : register(b0)
{
    uint  lWidth, lHeight, lSlice, _lpad;
};

[numthreads(8, 8, 1)]
void ExtractLumCentered(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= lWidth || dtid.y >= lHeight) return;
    float4 d = gDiffuse.Load(int4(dtid.xy, lSlice, 0));
    float lum = dot(d.rgb, float3(0.299, 0.587, 0.114));
    // Raw luminance (no -0.5 offset) — downstream MinMax reduction finds
    // the actual midpoint of this slice, so each slice's full range maps
    // to [0, 1] regardless of where its mean sits.
    gLumOut[dtid.xy] = float2(lum, 0.0);
}

// ============= Reduce MIN and MAX of luminance =============
// Min-max normalization gives "all textures the same displacement range"
// behavior — each slice's actual lum range maps to [0, 1], no clipping.
// Trick: for non-negative floats, asuint(x) is monotonic, so InterlockedMin
// and InterlockedMax on uint give correct float-min and float-max.
Texture2D<float2>             mmH      : register(t0);
RWStructuredBuffer<uint>      mmMinBuf : register(u0);
RWStructuredBuffer<uint>      mmMaxBuf : register(u1);

cbuffer MinMaxParams : register(b0)
{
    uint  mmWidth, mmHeight, _mmpad0, _mmpad1;
};

groupshared uint groupMin;
groupshared uint groupMmMax;

[numthreads(1024, 1, 1)]
void ReduceMinMax(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    if (gtid.x == 0)
    {
        groupMin    = 0xFFFFFFFFu;
        groupMmMax  = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint baseIdx = gid.x * 4096u + gtid.x * 4u;
    uint localMin = 0xFFFFFFFFu;
    uint localMax = 0;
    [unroll]
    for (uint kk = 0; kk < 4u; kk++)
    {
        uint idx = baseIdx + kk;
        if (idx < mmWidth * mmHeight)
        {
            uint x = idx % mmWidth;
            uint y = idx / mmWidth;
            float val = mmH[uint2(x, y)].x;
            // Handle negatives that may appear after HP roundtrip:
            // bias by adding 1.0 so the value is non-negative before bit-cast.
            // Range becomes [≈0, ≈2] which still has monotonic asuint.
            uint asInt = asuint(val + 1.0);
            localMin = min(localMin, asInt);
            localMax = max(localMax, asInt);
        }
    }
    InterlockedMin(groupMin, localMin);
    InterlockedMax(groupMmMax, localMax);
    GroupMemoryBarrierWithGroupSync();
    if (gtid.x == 0)
    {
        InterlockedMin(mmMinBuf[0], groupMin);
        InterlockedMax(mmMaxBuf[0], groupMmMax);
    }
}

// ============= Normalize FFT + Luminance, blend, write =============
// FFT branch: max-abs normalize (centered at 0 — the integrate step zeroes
// the DC bin so spatial mean is 0).
// Lum branch: min-max normalize. Each slice's actual midpoint maps to 0.5,
// each slice's full range maps to [0, 1] — same output range across all
// textures regardless of their natural luminance distribution. The min/max
// values were captured BIASED by +1.0 (to dodge the asuint sign issue for
// post-HP negatives), so we un-bias here.
Texture2D<float2>             nFft       : register(t0);
StructuredBuffer<uint>        nMaxFft    : register(t1);
Texture2D<float2>             nLum       : register(t2);
StructuredBuffer<uint>        nMaxLum    : register(t3);
StructuredBuffer<uint>        nMinLum    : register(t4);
RWTexture2DArray<float>       nOut       : register(u0);

cbuffer NormalizeParams : register(b0)
{
    uint  nWidth, nHeight, nSlice;
    float nMix;  // 0 = pure FFT, 1 = pure luminance
};

[numthreads(8, 8, 1)]
void Normalize(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= nWidth || dtid.y >= nHeight) return;
    float fft    = nFft[dtid.xy].x;
    float lum    = nLum[dtid.xy].x;
    float maxFft = max(asfloat(nMaxFft[0]), 1e-9);
    float lumMin = asfloat(nMinLum[0]) - 1.0;   // un-bias
    float lumMax = asfloat(nMaxLum[0]) - 1.0;
    float lumMid    = (lumMin + lumMax) * 0.5;
    float lumHalfR  = max((lumMax - lumMin) * 0.5, 1e-9);
    float fftN   = fft / maxFft;
    float lumN   = (lum - lumMid) / lumHalfR;
    float h      = lerp(fftN, lumN, nMix);
    nOut[uint3(dtid.xy, nSlice)] = saturate(0.5 + h * 0.5);
}

// ============= Standalone Gaussian high-pass =============
// Same attenuation curve as Integrate's optional HP block, but operates
// directly on a spectrum buffer without redoing the Frankot-Chellappa
// quotient. Used by the "flipped" pipeline where the HP runs AFTER the
// max-reduction so it doesn't lose its visual effect to a post-normalize.
Texture2D<float2>    hpIn  : register(t0);
RWTexture2D<float2>  hpOut : register(u0);

cbuffer HighPassParams : register(b0)
{
    uint  hpWidth, hpHeight;
    float hpCutoff;
    float hpStrength;
};

[numthreads(8, 8, 1)]
void HighPass(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= hpWidth || dtid.y >= hpHeight) return;
    float u = (dtid.x < hpWidth/2)  ? float(dtid.x) : (float(dtid.x) - float(hpWidth));
    float v = (dtid.y < hpHeight/2) ? float(dtid.y) : (float(dtid.y) - float(hpHeight));
    float atten = 1.0;
    if (hpStrength > 0.0 && hpCutoff > 0.0)
        atten = 1.0 - hpStrength * exp(-(u*u + v*v) / (2.0 * hpCutoff * hpCutoff));
    hpOut[dtid.xy] = hpIn[dtid.xy] * atten;
}

// ============= Init max buffer to zero =============
RWStructuredBuffer<uint> mInit : register(u0);

[numthreads(1, 1, 1)]
void InitMax(uint3 dtid : SV_DispatchThreadID)
{
    mInit[0] = 0;
}
)HLSL";

ID3D11HullShader*         gHs            = nullptr;
ID3D11DomainShader*       gDs            = nullptr;
ID3D11ComputeShader*      gDecodeNormalsCs = nullptr;
ID3D11SamplerState*       gHeightSampler = nullptr;
ID3D11Buffer*             gControlCb     = nullptr;
ID3D11RasterizerState*    gWireframeRs   = nullptr;  // lazy-created for wireframe debug modes
Controls                  gControls;

// GPU bake compute shaders (one per pipeline stage).
ID3D11ComputeShader*      gExtractCs     = nullptr;
ID3D11ComputeShader*      gFftCs         = nullptr;
ID3D11ComputeShader*      gIntegrateCs   = nullptr;
ID3D11ComputeShader*      gReduceCs      = nullptr;
ID3D11ComputeShader*      gNormalizeCs   = nullptr;
ID3D11ComputeShader*      gInitMaxCs     = nullptr;
ID3D11ComputeShader*      gExtractLumCs  = nullptr;
ID3D11ComputeShader*      gHighPassCs    = nullptr;
ID3D11ComputeShader*      gReduceMinMaxCs = nullptr;

// Working buffers for the GPU bake. R32G32_FLOAT 2K x 2K — 32MB each.
// Reused across slices, allocated once at Init for FFT_N=2048.
struct GpuBakeBuffers
{
    ID3D11Texture2D*           tex[3]      = { nullptr, nullptr, nullptr };
    ID3D11ShaderResourceView*  srv[3]      = { nullptr, nullptr, nullptr };
    ID3D11UnorderedAccessView* uav[3]      = { nullptr, nullptr, nullptr };
    ID3D11Buffer*              maxBuf      = nullptr;
    ID3D11ShaderResourceView*  maxSrv      = nullptr;
    ID3D11UnorderedAccessView* maxUav      = nullptr;
    ID3D11Buffer*              maxBufLum   = nullptr;
    ID3D11ShaderResourceView*  maxSrvLum   = nullptr;
    ID3D11UnorderedAccessView* maxUavLum   = nullptr;
    ID3D11Buffer*              minBufLum   = nullptr;
    ID3D11ShaderResourceView*  minSrvLum   = nullptr;
    ID3D11UnorderedAccessView* minUavLum   = nullptr;
    ID3D11Buffer*              cbExtract   = nullptr;
    ID3D11Buffer*              cbFft       = nullptr;
    ID3D11Buffer*              cbIntegrate = nullptr;
    ID3D11Buffer*              cbReduce    = nullptr;
    ID3D11Buffer*              cbNormalize = nullptr;
    UINT                       width       = 0;
    UINT                       height      = 0;
};
GpuBakeBuffers gGpuBake;

// Per-source-resource cache. Key = the Kenshi normalMaps Texture2D pointer.
// Value = a parallel Texture2DArray we created with the same dims, R16_UNORM,
// holding heights derived from each normal-map slice. Created lazily on first
// sighting via the compute shader bake.
struct HeightArray
{
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};
std::unordered_map<ID3D11Resource*, HeightArray> gHeightArrays;
// Shared heightArr — set on first bake. All subsequent terrain chunks bind
// this same SRV → identical height at the same world position across chunks
// → no chunk seams. Trade-off: same height pattern for every biome.
ID3D11ShaderResourceView* gSharedHeightArr = nullptr;

// Per-frame chunk grid: maps integer (gridX, gridZ) → heightArray SRV.
// Populated as terrain draws come in; allows neighbor lookups in O(1).
// Definition hoisted here so RebakeAll() can clear it (the SRV pointers
// it stores otherwise dangle when gHeightArrays is wiped).
struct ChunkKey { int x; int z; bool operator==(const ChunkKey& o) const { return x == o.x && z == o.z; } };
struct ChunkKeyHash { size_t operator()(const ChunkKey& k) const { return (size_t)k.x * 73856093u ^ (size_t)k.z * 19349663u; } };
std::unordered_map<ChunkKey, ID3D11ShaderResourceView*, ChunkKeyHash> gChunkGrid;
float gChunkGridUnit = 0.0f;


// Captured PS bytecode by PS pointer, for cbuffer-layout reflection.
std::unordered_map<ID3D11PixelShader*, std::vector<uint8_t>> gPsBytecode;

// Per-PS blend level: -1 not main terrain (e.g. simple_fs distant), 0 base
// only, 1/2/3 with extra normalMaps1/2/3 sets. We tessellate level 0 only
// for now since higher levels need their own baked heightmap arrays.
std::unordered_map<ID3D11PixelShader*, int> gPsBlendLevel;

// Captured BLEND1/2/3 channel-index #defines (0..3 = R/G/B/A) per PS.
// OnTerrainPsCompiled stashes by bytecode hash; OnPixelShaderCreated
// re-hashes the bytecode and moves the entry into the PS-pointer map.
struct PsBlendDefines { int b1 = -1, b2 = -1, b3 = -1; };
static std::unordered_map<uint64_t, PsBlendDefines>             gBlendDefsByHash;
static std::unordered_map<ID3D11PixelShader*, PsBlendDefines>   gPsBlendDefs;

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
    ID3D11ShaderResourceView* dsSrv0  = nullptr;
    ID3D11ShaderResourceView* dsSrv1  = nullptr;
    ID3D11ShaderResourceView* dsSrv2  = nullptr;  // -X neighbor heightArray
    ID3D11ShaderResourceView* dsSrv3  = nullptr;  // +X neighbor
    ID3D11ShaderResourceView* dsSrv4  = nullptr;  // -Z neighbor
    ID3D11ShaderResourceView* dsSrv5  = nullptr;  // +Z neighbor
    ID3D11ShaderResourceView* dsSrv6  = nullptr;  // mirrored PS diffuseMaps
    ID3D11ShaderResourceView* dsSrv7  = nullptr;  // mirrored PS colourMap
    ID3D11ShaderResourceView* dsSrv8  = nullptr;  // mirrored PS diffuseMaps1
    ID3D11ShaderResourceView* dsSrv9  = nullptr;  // mirrored PS diffuseMaps2
    ID3D11ShaderResourceView* dsSrv10 = nullptr;  // mirrored PS diffuseMaps3
    ID3D11ShaderResourceView* dsSrv11 = nullptr;  // mirrored PS blendMap
    ID3D11SamplerState*       dsSamp0 = nullptr;
    ID3D11ShaderResourceView* psSrv12 = nullptr;
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
            target,
            errors ? (const char*)errors->GetBufferPointer() : "no error blob");
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

} // anonymous namespace

Controls* GetControls() { return &gControls; }

// Bake-time high-pass parameters. Cutoff in cycles/image (Gaussian sigma in
// frequency domain) — lower = suppress wider macro features; 0 disables.
// Strength 0..1 controls how aggressively low-freqs are attenuated.
static float gBakeHighPassCutoff   = 0.0f;
static float gBakeHighPassStrength = 1.0f;

// Bake-time normal-vs-luminance blend. 0 = pure FFT-from-normal,
// 1 = pure luminance-as-height (bright = raised). 0.5 default works well
// in practice — luminance helps rocky surfaces (where the normal map is
// flat but the diffuse has depth cues), while the FFT path keeps sand
// dunes accurate (luminance alone gets dunes wrong).
static float gBakeLumMix           = 0.5f;

float GetBakeHighPassCutoff()   { return gBakeHighPassCutoff; }
float GetBakeHighPassStrength() { return gBakeHighPassStrength; }
void  SetBakeHighPass(float cutoff, float strength)
{
    gBakeHighPassCutoff   = cutoff;
    gBakeHighPassStrength = strength;
}

float GetBakeLumMix() { return gBakeLumMix; }
void  SetBakeLumMix(float v) { gBakeLumMix = v; }

// Master enable flag — when off, IsTerrainShaderBound returns false and the
// HS/DS path is bypassed entirely. The bake never runs (heightArrays stay
// uncached) so memory cost goes to zero too.
static bool gEnabled = true;
bool GetEnabled() { return gEnabled; }
void SetEnabled(bool enabled) { gEnabled = enabled; }

// Per-frame GPU timing. Each tess draw is bracketed with timestamp queries
// under one disjoint per frame; we read the previous frame's results to
// avoid a CPU stall. The plugin polls GetGpuTimeMs() each frame.
struct TimerFrame
{
    ID3D11Query*           disjoint = nullptr;
    std::vector<ID3D11Query*> tsBegin;
    std::vector<ID3D11Query*> tsEnd;
    UINT                   used = 0;       // begin/end pairs issued this frame
    bool                   issued = false; // disjoint+queries End'd; safe to read
};
static const int kTimerFrames     = 3;     // ring-buffer to avoid stalls
static const int kMaxDrawsPerFrame = 256;  // cap pool size
static TimerFrame gTimerFrames[kTimerFrames];
static int        gTimerCur     = 0;       // currently-recording frame
static float      gGpuTimeMs    = 0.0f;    // last-completed frame's total

float GetGpuTimeMs() { return gGpuTimeMs; }

void RebakeAll()
{
    // Alias entries (tex == nullptr) share their srv with the owning entry;
    // only release owning ones so we don't double-release the shared SRV.
    for (auto& kv : gHeightArrays)
    {
        if (kv.second.tex)
        {
            if (kv.second.srv) kv.second.srv->Release();
            kv.second.tex->Release();
        }
    }
    gHeightArrays.clear();
    gSharedHeightArr = nullptr;
    // Critical: gChunkGrid stores raw SRV pointers that just got released.
    // Leaving them in place would let the next terrain draw bind dangling
    // pointers to DS slots → crash. Cleared, the first frame after rebake
    // has no neighbors (cross-region seams briefly visible) but the grid
    // re-populates within 1-2 frames as draws come in.
    gChunkGrid.clear();
    Log("TerrainTess: cleared height array cache; next terrain draw will rebake");
}

// Called by D3D11Hook::ResetFrameState at the start of each frame. Closes
// the in-flight disjoint, harvests the OLDEST ring entry's data into
// gGpuTimeMs, and advances the ring pointer so the next frame writes into
// the freed slot.
void OnFrameEnd()
{
    // We DON'T clear gChunkGrid here — entries persist across frames so
    // neighbor lookups succeed on the very first draw of each frame
    // (otherwise the first few draws would have no neighbors yet, producing
    // visible seams that vanish on subsequent frames). Stale entries point
    // to heightArrays in gHeightArrays which are kept for the session, so
    // their SRVs remain valid. (gridX, gridZ) keys are unique per chunk
    // so new chunks don't collide with old ones.

    ID3D11DeviceContext* ctx = D3D11Hook::gContext;
    if (!ctx) return;

    auto& cur = gTimerFrames[gTimerCur];
    if (cur.used > 0 && cur.disjoint)
    {
        ctx->End(cur.disjoint);
        cur.issued = true;
    }

    // Advance ring; the slot we move to is the one we'll record into next
    // frame, but it may also be the OLDEST already-issued slot whose data
    // is now ready.
    int nextIdx = (gTimerCur + 1) % kTimerFrames;
    auto& nxt = gTimerFrames[nextIdx];
    if (nxt.issued)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        if (ctx->GetData(nxt.disjoint, &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint && dj.Frequency > 0)
        {
            double total = 0.0;
            for (UINT i = 0; i < nxt.used; i++)
            {
                UINT64 b = 0, e = 0;
                if (ctx->GetData(nxt.tsBegin[i], &b, sizeof(b), 0) == S_OK &&
                    ctx->GetData(nxt.tsEnd[i],   &e, sizeof(e), 0) == S_OK)
                {
                    total += 1000.0 * (double)(e - b) / (double)dj.Frequency;
                }
            }
            gGpuTimeMs = (float)total;
        }
        else
        {
            // Disjoint or data not ready — keep last value (avoid jittering to 0).
        }
        nxt.issued = false;
    }
    nxt.used = 0;
    gTimerCur = nextIdx;
}

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

// === Per-draw chunk-position snoop (for cross-region neighbor binding) ===
//
// Each terrain VS has a $Globals cbuffer holding worldMatrix (4x4) and
// overlayData (float4 = chunk world bounds: x0,z0,x1,z1). We reflect each
// terrain VS at create time to find those byte offsets in its cbuffer
// layout (ordering can differ from source due to optimizer-driven uniform
// stripping). At draw time we look up the bound VS's offsets, read the
// snooped cbuffer data captured by Map/Unmap hooks, and extract the chunk
// origin + size in world units.

struct VsCbLayout
{
    int worldMatOffset   = -1;  // byte offset of float4x4 worldMatrix
    int overlayDataOffset = -1; // byte offset of float4 overlayData
    int cbSize            = 0;
};
// Keyed by VS pointer (we get this in OnVertexShaderCreated and have it
// at draw time via VSGetShader).
std::unordered_map<ID3D11VertexShader*, VsCbLayout> gVsCbLayout;

// Per-cbuffer snooped data: latest write captured by Unmap.
std::unordered_map<ID3D11Buffer*, std::vector<uint8_t>> gCbufSnap;
std::unordered_map<ID3D11Buffer*, void*> gCbufMapped;
std::unordered_set<ID3D11Buffer*> gCbufTracked;
std::atomic<int> gCbufTrackedSize{0};

void OnVertexShaderCreated(const void* bytecode, size_t size, ID3D11VertexShader* vs)
{
    if (!bytecode || !vs || size == 0) return;
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, (void**)&refl)) || !refl)
        return;

    VsCbLayout layout;
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.ConstantBuffers; i++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC bd = {};
        cb->GetDesc(&bd);
        for (UINT v = 0; v < bd.Variables; v++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vd = {};
            var->GetDesc(&vd);
            if (!vd.Name) continue;
            if (strcmp(vd.Name, "worldMatrix") == 0)
            {
                layout.worldMatOffset = (int)vd.StartOffset;
                layout.cbSize = (int)bd.Size;
            }
            else if (strcmp(vd.Name, "overlayData") == 0)
            {
                layout.overlayDataOffset = (int)vd.StartOffset;
            }
        }
    }
    refl->Release();

    // Only store if we found both — otherwise this isn't the terrain VS
    // we expect, and per-draw lookups should fall back gracefully.
    if (layout.worldMatOffset >= 0 && layout.overlayDataOffset >= 0)
    {
        gVsCbLayout[vs] = layout;
        Log("TerrainTess: terrain VS %p layout: worldMat@%d overlayData@%d cbSize=%d",
            vs, layout.worldMatOffset, layout.overlayDataOffset, layout.cbSize);
    }
}

void OnCbufferMap(ID3D11Buffer* buf, void* dataPtr)
{
    if (gCbufTrackedSize.load(std::memory_order_acquire) == 0) return;
    if (gCbufTracked.count(buf))
        gCbufMapped[buf] = dataPtr;
}

void OnCbufferUnmap(ID3D11Buffer* buf)
{
    if (gCbufTrackedSize.load(std::memory_order_acquire) == 0) return;
    auto it = gCbufMapped.find(buf);
    if (it == gCbufMapped.end()) return;
    auto& dst = gCbufSnap[buf];
    // 256 bytes is enough for the terrain VS cbuffer (~240 bytes typical).
    if (dst.size() < 256) dst.resize(256);
    memcpy(dst.data(), it->second, 256);
    gCbufMapped.erase(it);
}

// Mark a cbuffer as a terrain VS cbuffer worth snooping.
static void TrackCbuffer(ID3D11Buffer* buf)
{
    if (gCbufTracked.insert(buf).second)
        gCbufTrackedSize.fetch_add(1, std::memory_order_release);
}

// Extract chunk origin (x, z) and size (sx, sz) from a snooped VS cbuffer
// using the layout we reflected for `vs`. Returns false if no data is
// available yet (first frame) or the VS isn't a known terrain VS.
static bool GetChunkBoundsFromVS(ID3D11VertexShader* vs, ID3D11Buffer* cb,
                                  float& outOriginX, float& outOriginZ,
                                  float& outSizeX,   float& outSizeZ)
{
    auto layoutIt = gVsCbLayout.find(vs);
    if (layoutIt == gVsCbLayout.end()) return false;
    auto snapIt = gCbufSnap.find(cb);
    if (snapIt == gCbufSnap.end() || snapIt->second.size() < 256) return false;

    const uint8_t* data = snapIt->second.data();
    int wmOff = layoutIt->second.worldMatOffset;
    int odOff = layoutIt->second.overlayDataOffset;

    // worldMatrix translation at offsets +48..56 (4th row in row-major == 4th
    // column in column-major: both layouts put translation x/y/z at bytes
    // 48,52,56 within the matrix).
    const float* tr = (const float*)(data + wmOff + 48);
    // overlayData = float4(x0, z0, x1, z1) in CHUNK-LOCAL space (matches the
    // VS's `position` input, which is local). Size = max - min.
    const float* od = (const float*)(data + odOff);

    // We only expose the worldMatrix translation here — used by the per-frame
    // chunk grid (gChunkGrid) for neighbor lookup. Chunk-local UV in the
    // shaders is derived via frac(worldPos.xz / chunkSizeWorld) using the
    // user-tunable chunkSizeWorld slider, since overlayData is region-scoped
    // and Kenshi exposes no per-draw per-chunk extent.
    outOriginX = tr[0];
    outOriginZ = tr[2];
    outSizeX   = od[2] - od[0];
    outSizeZ   = od[3] - od[1];
    return true;
}

// Reflect the bytecode and log its input + output signatures. Used to verify
// that VS output → HS input → DS output → PS input semantic chain is intact.
// Silent pipeline failures from signature mismatches are very hard to diagnose
// without this kind of introspection.
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

// In-place 1D Cooley-Tukey radix-2 FFT. n MUST be a power of 2.
// inverse=true uses inverse twiddles (caller normalizes by n).
static void Fft1D(std::complex<double>* a, size_t n, bool inverse)
{
    size_t j = 0;
    for (size_t i = 1; i < n; i++)
    {
        size_t bit = n >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const double pi = 3.14159265358979323846;
    for (size_t len = 2; len <= n; len <<= 1)
    {
        double ang = (inverse ? 2.0 : -2.0) * pi / (double)len;
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; k++)
            {
                std::complex<double> u = a[i + k];
                std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// 2D FFT in-place: row FFTs then column FFTs.
static void Fft2D(std::complex<double>* a, size_t w, size_t h, bool inverse)
{
    for (size_t y = 0; y < h; y++)
        Fft1D(a + y * w, w, inverse);
    std::vector<std::complex<double>> col(h);
    for (size_t x = 0; x < w; x++)
    {
        for (size_t y = 0; y < h; y++) col[y] = a[y * w + x];
        Fft1D(col.data(), h, inverse);
        for (size_t y = 0; y < h; y++) a[y * w + x] = col[y];
    }
}

// Frankot-Chellappa: integrate gradient field (p, q) → height. Optional
// frequency-domain Gaussian high-pass attenuates macro features (kills the
// "characters float over hills" artifact). Output is mean-centered to 0.5
// with symmetric percentile clipping so amplitude is signed around mid.
static void FrankotChellappa(const float* p, const float* q,
                              size_t w, size_t h,
                              std::vector<uint16_t>& heightOutU16)
{
    size_t n = w * h;
    std::vector<std::complex<double>> Fp(n), Fq(n);
    for (size_t i = 0; i < n; i++)
    {
        Fp[i] = std::complex<double>(p[i], 0.0);
        Fq[i] = std::complex<double>(q[i], 0.0);
    }
    Fft2D(Fp.data(), w, h, false);
    Fft2D(Fq.data(), w, h, false);

    const std::complex<double> imagJ(0.0, 1.0);
    std::vector<std::complex<double>> H(n);
    for (size_t y = 0; y < h; y++)
    {
        double v = (y < h / 2) ? (double)y : ((double)y - (double)h);
        for (size_t x = 0; x < w; x++)
        {
            double u = (x < w / 2) ? (double)x : ((double)x - (double)w);
            size_t k = y * w + x;
            double denom = u * u + v * v;
            if (denom < 1e-12)
                H[k] = std::complex<double>(0, 0);
            else
                H[k] = (-imagJ * u * Fp[k] + -imagJ * v * Fq[k]) / denom;
        }
    }
    // Optional Gaussian high-pass: 1 - s * exp(-f² / 2σ²). Suppresses macro
    // features (which manifest as characters floating/sinking over slow
    // height variations). σ is in cycles/image — σ=8 attenuates wavelengths
    // longer than ~image_size/8.
    if (gBakeHighPassCutoff > 0.0f && gBakeHighPassStrength > 0.0f)
    {
        double sigma = (double)gBakeHighPassCutoff;
        double s     = (double)gBakeHighPassStrength;
        double inv2sig2 = 1.0 / (2.0 * sigma * sigma);
        for (size_t y = 0; y < h; y++)
        {
            double v = (y < h / 2) ? (double)y : ((double)y - (double)h);
            for (size_t x = 0; x < w; x++)
            {
                double u = (x < w / 2) ? (double)x : ((double)x - (double)w);
                size_t k = y * w + x;
                double atten = 1.0 - s * std::exp(-(u*u + v*v) * inv2sig2);
                H[k] *= atten;
            }
        }
    }

    Fft2D(H.data(), w, h, true);

    // Real part / n.
    std::vector<float> heights(n);
    double inv_n = 1.0 / (double)n;
    for (size_t i = 0; i < n; i++)
        heights[i] = (float)(H[i].real() * inv_n);

    // Mean-center so the heightmap averages to 0.5 after normalize. The DC
    // bin was zeroed before integration, but the high-pass + numerical noise
    // can leave a residual offset.
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += heights[i];
    float mean = (float)(sum / (double)n);
    for (size_t i = 0; i < n; i++) heights[i] -= mean;

    // Symmetric percentile clip: pick the 99.5%-ile of |H| as the half-range.
    // Maps mean → 0.5 and ±range → 0 / 1.
    std::vector<float> absH(n);
    for (size_t i = 0; i < n; i++) absH[i] = std::abs(heights[i]);
    std::sort(absH.begin(), absH.end());
    float halfRange = absH[(size_t)(0.995 * n)];
    if (halfRange < 1e-9f) halfRange = 1e-9f;

    heightOutU16.resize(n);
    for (size_t i = 0; i < n; i++)
    {
        float v = 0.5f + heights[i] / (2.0f * halfRange);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        heightOutU16[i] = (uint16_t)(v * 65535.0f + 0.5f);
    }
}

// Decode a BC3 Texture2DArray to RGBA8 via the decode CS, copy to a staging
// texture, and dump every slice to a binary file (header u32 W, H, slices then
// raw RGBA8 bytes). Used to compare diffuse + normal + heightmap offline.
static void DumpDecodedArray(ID3D11Device* device,
                              ID3D11DeviceContext* ctx,
                              ID3D11ShaderResourceView* srcSrv,
                              const char* filename)
{
    if (!gDecodeNormalsCs || !srcSrv) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    srcSrv->GetDesc(&sd);
    if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY) return;
    UINT slices = sd.Texture2DArray.ArraySize;

    ID3D11Resource* res = nullptr;
    srcSrv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D* srcTex = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex);
    res->Release();
    if (FAILED(hr)) return;
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTex->GetDesc(&srcDesc);
    srcTex->Release();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = srcDesc.Width;
    td.Height = srcDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = slices;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* decoded = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &decoded))) return;

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    ud.Texture2DArray.ArraySize = slices;
    ID3D11UnorderedAccessView* uav = nullptr;
    if (FAILED(device->CreateUnorderedAccessView(decoded, &ud, &uav)))
    { decoded->Release(); return; }

    ID3D11ComputeShader*       saved_cs   = nullptr;
    ID3D11ShaderResourceView*  saved_srv  = nullptr;
    ID3D11UnorderedAccessView* saved_uav  = nullptr;
    ID3D11SamplerState*        saved_samp = nullptr;
    ctx->CSGetShader(&saved_cs, nullptr, 0);
    ctx->CSGetShaderResources(0, 1, &saved_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &saved_uav);
    ctx->CSGetSamplers(0, 1, &saved_samp);

    UINT initial = 0;
    ctx->CSSetShader(gDecodeNormalsCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srcSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &initial);
    ctx->CSSetSamplers(0, 1, &gHeightSampler);
    ctx->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, slices);

    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetShader(saved_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &saved_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &saved_uav, nullptr);
    ctx->CSSetSamplers(0, 1, &saved_samp);
    if (saved_cs)   saved_cs->Release();
    if (saved_srv)  saved_srv->Release();
    if (saved_uav)  saved_uav->Release();
    if (saved_samp) saved_samp->Release();

    D3D11_TEXTURE2D_DESC stagingDesc = td;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
    { uav->Release(); decoded->Release(); return; }
    ctx->CopyResource(staging, decoded);

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&DumpDecodedArray, &self);
    char dllPath[MAX_PATH] = {};
    GetModuleFileNameA(self, dllPath, MAX_PATH);
    std::string base(dllPath);
    auto slash = base.find_last_of("\\/");
    std::string path = (slash != std::string::npos ? base.substr(0, slash + 1) : "")
                       + filename;
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (f)
    {
        uint32_t hdr[3] = { srcDesc.Width, srcDesc.Height, slices };
        fwrite(hdr, 4, 3, f);
        for (UINT s = 0; s < slices; s++)
        {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(staging, s, D3D11_MAP_READ, 0, &m)))
            {
                for (UINT y = 0; y < srcDesc.Height; y++)
                    fwrite((const char*)m.pData + (size_t)y * m.RowPitch, 1, srcDesc.Width * 4, f);
                ctx->Unmap(staging, s);
            }
        }
        fclose(f);
        Log("TerrainTess: dumped decoded RGBA8 to %s", path.c_str());
    }
    staging->Release();
    uav->Release();
    decoded->Release();
}

// Helper: upload a 16-byte cbuffer payload to an existing dynamic cb.
static void UploadCbuf(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, const void* data)
{
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, data, 16);
        ctx->Unmap(cb, 0);
    }
}

// Full-GPU bake: same Frankot-Chellappa math as the CPU path, but every
// stage runs on the GPU. Returns the final R16_FLOAT heightArray SRV, or
// nullptr if size != 2048 (caller falls back to CPU).
//
// `decodedDiffuseSrv` may be null — when null, lum mix is forced to 0
// (FFT-only, identical to the older bake).
static ID3D11ShaderResourceView* BakeHeightArrayGpu(ID3D11Device* device,
                                                     ID3D11DeviceContext* ctx,
                                                     ID3D11ShaderResourceView* decodedSrv,
                                                     ID3D11ShaderResourceView* decodedDiffuseSrv,
                                                     UINT W, UINT H)
{
    if (W != 2048 || H != 2048)         return nullptr;
    if (!gExtractCs   || !gFftCs)       return nullptr;
    if (!gIntegrateCs || !gReduceCs)    return nullptr;
    if (!gNormalizeCs || !gInitMaxCs)   return nullptr;
    if (!gGpuBake.tex[0])               return nullptr;

    // Output: R16_FLOAT with full mip chain. Mips suppress sub-tessellation
    // aliasing — at low tess density a pixel-sized heightmap spike would
    // otherwise displace one tess vertex while neighbors sit at base height,
    // producing a sharp pointy artifact. SampleLevel with a tess-density-
    // matched mip averages over a neighborhood and smooths the spike out.
    //
    // RENDER_TARGET + GENERATE_MIPS are required for ID3D11DeviceContext::
    // GenerateMips. UAV writes still target mip 0 by default; mips 1..N
    // are filled by GenerateMips after all 6 slices are written.
    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width      = W;
    dstDesc.Height     = H;
    dstDesc.MipLevels  = 0;   // full chain
    dstDesc.ArraySize  = 6;
    dstDesc.Format     = DXGI_FORMAT_R16_FLOAT;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage      = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
                       | D3D11_BIND_RENDER_TARGET;
    dstDesc.MiscFlags  = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ID3D11Texture2D* heightTex = nullptr;
    if (FAILED(device->CreateTexture2D(&dstDesc, nullptr, &heightTex))) return nullptr;

    // Default SRV (mip range = -1) covers the full mip chain.
    ID3D11ShaderResourceView* heightSrv = nullptr;
    if (FAILED(device->CreateShaderResourceView(heightTex, nullptr, &heightSrv)))
    { heightTex->Release(); return nullptr; }

    // UAV defaults to mip 0 — what the normalize CS writes.
    D3D11_UNORDERED_ACCESS_VIEW_DESC heightUavDesc = {};
    heightUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    heightUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    heightUavDesc.Texture2DArray.ArraySize = 6;
    ID3D11UnorderedAccessView* heightUav = nullptr;
    if (FAILED(device->CreateUnorderedAccessView(heightTex, &heightUavDesc, &heightUav)))
    { heightSrv->Release(); heightTex->Release(); return nullptr; }

    // Save CS state we trample. Restored at end.
    ID3D11ComputeShader*       savedCs       = nullptr;
    ID3D11ShaderResourceView*  savedSrv0     = nullptr;
    ID3D11ShaderResourceView*  savedSrv1     = nullptr;
    ID3D11UnorderedAccessView* savedUav0     = nullptr;
    ID3D11UnorderedAccessView* savedUav1     = nullptr;
    ID3D11Buffer*              savedCb0      = nullptr;
    ID3D11SamplerState*        savedSamp     = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, 0);
    ctx->CSGetShaderResources(0, 1, &savedSrv0);
    ctx->CSGetShaderResources(1, 1, &savedSrv1);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav0);
    ctx->CSGetUnorderedAccessViews(1, 1, &savedUav1);
    ctx->CSGetConstantBuffers(0, 1, &savedCb0);
    ctx->CSGetSamplers(0, 1, &savedSamp);

    UINT initialCounts[2] = { 0, 0 };
    ID3D11UnorderedAccessView* nullUavs[2] = { nullptr, nullptr };
    ID3D11ShaderResourceView*  nullSrvs[2] = { nullptr, nullptr };

    ID3D11ShaderResourceView*  bufSrv[3] = { gGpuBake.srv[0], gGpuBake.srv[1], gGpuBake.srv[2] };
    ID3D11UnorderedAccessView* bufUav[3] = { gGpuBake.uav[0], gGpuBake.uav[1], gGpuBake.uav[2] };
    const int P = 0, Q = 1, S = 2; // P-buffer, Q-buffer, scratch

    // Skip the FFT-from-normal pipeline entirely when the user has set
    // lumMix to (essentially) 1 — its result would be multiplied by 0 in
    // the normalize blend anyway, so all that work is wasted. Saves
    // ~15 dispatches per slice and ~50% bake time.
    const bool lumOnly = (gBakeLumMix > 0.999f) && (decodedDiffuseSrv != nullptr);

    // Per-slice pipeline.
    for (UINT slice = 0; slice < 6; slice++)
    {
        // (1) Extract gradients from decoded normal slice → bufP, bufQ.
        if (!lumOnly)
        {
            uint32_t cb[4] = { W, H, slice, 0 };
            UploadCbuf(ctx, gGpuBake.cbExtract, cb);
            ctx->CSSetShader(gExtractCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &decodedSrv);
            ID3D11UnorderedAccessView* uavs[2] = { bufUav[P], bufUav[Q] };
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbExtract);
            ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
            ctx->CSSetShaderResources(0, 1, nullSrvs);
        }

        // FFT helper: read from src, write to dst (different textures so SRV/UAV
        // don't alias). Each dispatch is a full 1D FFT over its axis.
        auto fft = [&](uint32_t axis, uint32_t inverse, int srcIdx, int dstIdx)
        {
            uint32_t cb[4] = { axis, inverse, 0, 0 };
            UploadCbuf(ctx, gGpuBake.cbFft, cb);
            ctx->CSSetShader(gFftCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &bufSrv[srcIdx]);
            ctx->CSSetUnorderedAccessViews(0, 1, &bufUav[dstIdx], initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbFft);
            // Dispatch one group per "line" (row for axis=0, column for axis=1).
            // Group has FFT_N/2 = 1024 threads internally.
            UINT lines = (axis == 0) ? H : W;
            ctx->Dispatch(lines, 1, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ctx->CSSetShaderResources(0, 1, nullSrvs);
        };

        if (!lumOnly)
        {
            // (2-3) Forward FFT 2D on P: P → S (rows) → P (cols).
            fft(0, 0, P, S);
            fft(1, 0, S, P);
            // (4-5) Forward FFT 2D on Q: Q → S (rows) → Q (cols).
            fft(0, 0, Q, S);
            fft(1, 0, S, Q);
        }

        if (!lumOnly)
        {
            // (6) Integrate: bufP=Fp, bufQ=Fq → bufS = H spectrum.
            // Note: HP is NOT applied here — we want max(|H|) measured BEFORE
            // attenuation so the post-HP normalize uses the no-HP scale and
            // the HP visibly reduces overall amplitude.
            struct { uint32_t w, h; float cutoff, strength; } cb;
            cb.w = W; cb.h = H;
            cb.cutoff   = 0.0f;  // force no HP in integrate
            cb.strength = 0.0f;
            UploadCbuf(ctx, gGpuBake.cbIntegrate, &cb);
            ctx->CSSetShader(gIntegrateCs, nullptr, 0);
            ID3D11ShaderResourceView* srvs[2] = { bufSrv[P], bufSrv[Q] };
            ctx->CSSetShaderResources(0, 2, srvs);
            ctx->CSSetUnorderedAccessViews(0, 1, &bufUav[S], initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbIntegrate);
            ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ctx->CSSetShaderResources(0, 2, nullSrvs);

            // (7-8) Inverse FFT 2D on H: S → P (cols) → S (rows). After this,
            // bufS holds the real-valued height (no HP).
            fft(1, 1, S, P);
            fft(0, 1, P, S);
        }

        // Reduction helper (writes max-abs of bufSrv[srcIdx] into the given UAV).
        auto reduceMaxAbs = [&](int srcIdx, ID3D11UnorderedAccessView* maxUav)
        {
            // Init max to 0.
            ctx->CSSetShader(gInitMaxCs, nullptr, 0);
            ctx->CSSetUnorderedAccessViews(0, 1, &maxUav, initialCounts);
            ctx->Dispatch(1, 1, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

            uint32_t cb[4] = { W, H, 0, 0 };
            UploadCbuf(ctx, gGpuBake.cbReduce, cb);
            ctx->CSSetShader(gReduceCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &bufSrv[srcIdx]);
            ctx->CSSetUnorderedAccessViews(0, 1, &maxUav, initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbReduce);
            UINT groups = (W * H + 4095) / 4096;
            ctx->Dispatch(groups, 1, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ctx->CSSetShaderResources(0, 1, nullSrvs);
        };

        // (9-10) Reduce max(|FFT|) on the NO-HP height field. This max is
        // what we'll normalize the post-HP output against — preserving the
        // visible amplitude reduction the high-pass produces.
        if (!lumOnly)
            reduceMaxAbs(S, gGpuBake.maxUav);

        // (10b) If HP is enabled, re-FFT bufS, apply Gaussian high-pass,
        // and IFFT back. Skipped when HP disabled — pipeline cost is unchanged.
        if (!lumOnly && gBakeHighPassCutoff > 0.0f && gBakeHighPassStrength > 0.0f)
        {
            // Diagnostic: log once per slice so we can verify the HP is firing
            // with the expected parameters when the user moves the slider.
            if (slice == 0)
                Log("TerrainTess: HP active cutoff=%.2f strength=%.2f",
                    gBakeHighPassCutoff, gBakeHighPassStrength);
            // Forward 2D FFT on bufS: row → bufQ, col → bufS = re-spectrum.
            fft(0, 0, S, Q);
            fft(1, 0, Q, S);

            // Apply Gaussian HP attenuation. Output to bufQ (can't read+write
            // same texture in one dispatch).
            struct { uint32_t w, h; float cutoff, strength; } cb;
            cb.w = W; cb.h = H;
            cb.cutoff   = gBakeHighPassCutoff;
            cb.strength = gBakeHighPassStrength;
            UploadCbuf(ctx, gGpuBake.cbIntegrate, &cb); // reuses Integrate cb (same 16 bytes)
            ctx->CSSetShader(gHighPassCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &bufSrv[S]);
            ctx->CSSetUnorderedAccessViews(0, 1, &bufUav[Q], initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbIntegrate);
            ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ctx->CSSetShaderResources(0, 1, nullSrvs);

            // IFFT bufQ → bufP (col) → bufS (row). bufS now has H_HP.
            fft(1, 1, Q, P);
            fft(0, 1, P, S);
        }

        // (11) Extract centered luminance from decoded diffuse → bufP.
        // Skipped when no diffuse SRV — mix is then forced to 0.
        float effectiveMix = (decodedDiffuseSrv != nullptr) ? gBakeLumMix : 0.0f;
        // Track which buffer holds the final lum data — defaults to P, but
        // shifts to Q after the HP round-trip ends there.
        int lumBufIdx = P;
        if (decodedDiffuseSrv)
        {
            uint32_t cb[4] = { W, H, slice, 0 };
            UploadCbuf(ctx, gGpuBake.cbExtract, cb);  // reuse 16-byte cb
            ctx->CSSetShader(gExtractLumCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &decodedDiffuseSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &bufUav[P], initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbExtract);
            ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ctx->CSSetShaderResources(0, 1, nullSrvs);

            // (12) Apply HP round-trip on lum (if enabled). Final HP'd lum
            // lands in bufQ. Without HP, bufP holds raw lum unchanged.
            if (gBakeHighPassCutoff > 0.0f && gBakeHighPassStrength > 0.0f)
            {
                fft(0, 0, P, Q);
                fft(1, 0, Q, P);  // bufP = lum spectrum

                struct { uint32_t w, h; float cutoff, strength; } hpCb;
                hpCb.w = W; hpCb.h = H;
                hpCb.cutoff   = gBakeHighPassCutoff;
                hpCb.strength = gBakeHighPassStrength;
                UploadCbuf(ctx, gGpuBake.cbIntegrate, &hpCb);
                ctx->CSSetShader(gHighPassCs, nullptr, 0);
                ctx->CSSetShaderResources(0, 1, &bufSrv[P]);
                ctx->CSSetUnorderedAccessViews(0, 1, &bufUav[Q], initialCounts);
                ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbIntegrate);
                ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
                ctx->CSSetShaderResources(0, 1, nullSrvs);

                fft(1, 1, Q, P);
                fft(0, 1, P, Q);  // bufQ = HP'd lum
                lumBufIdx = Q;
            }

            // (13) Reduce min AND max on the FINAL lum buffer (post-HP if
            // applicable). The Normalize CS uses these to map each slice's
            // actual range to [0, 1] — same output range across all
            // textures, no clipping (relative-contrast normalization).
            const UINT minClear[4] = { 0xFFFFFFFFu, 0u, 0u, 0u };
            const UINT maxClear[4] = { 0u, 0u, 0u, 0u };
            ctx->ClearUnorderedAccessViewUint(gGpuBake.minUavLum, minClear);
            ctx->ClearUnorderedAccessViewUint(gGpuBake.maxUavLum, maxClear);
            {
                uint32_t cb[4] = { W, H, 0, 0 };
                UploadCbuf(ctx, gGpuBake.cbReduce, cb);
                ctx->CSSetShader(gReduceMinMaxCs, nullptr, 0);
                ctx->CSSetShaderResources(0, 1, &bufSrv[lumBufIdx]);
                ID3D11UnorderedAccessView* uavs[2] = { gGpuBake.minUavLum, gGpuBake.maxUavLum };
                ctx->CSSetUnorderedAccessViews(0, 2, uavs, initialCounts);
                ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbReduce);
                UINT groups = (W * H + 4095) / 4096;
                ctx->Dispatch(groups, 1, 1);
                ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
                ctx->CSSetShaderResources(0, 1, nullSrvs);
            }
        }

        // (14) Normalize: blend FFT (S, maxFft) with luminance (P, maxLum) by
        // effectiveMix, write to heightArray slice.
        {
            struct { uint32_t w, h, sliceIdx; float mix; } cb;
            cb.w = W; cb.h = H; cb.sliceIdx = slice; cb.mix = effectiveMix;
            UploadCbuf(ctx, gGpuBake.cbNormalize, &cb);
            ctx->CSSetShader(gNormalizeCs, nullptr, 0);
            // In lumOnly mode the FFT path was skipped, so bufS / maxBuf
            // contain stale data. Bind the lum SRVs at both slots so any
            // bleed (with mix not exactly 1) collapses to lum-only output
            // instead of garbage propagating through.
            ID3D11ShaderResourceView* fftSrvBind    = lumOnly ? bufSrv[lumBufIdx] : bufSrv[S];
            ID3D11ShaderResourceView* fftMaxSrvBind = lumOnly ? gGpuBake.maxSrvLum : gGpuBake.maxSrv;
            ID3D11ShaderResourceView* srvs[5] = {
                fftSrvBind, fftMaxSrvBind, bufSrv[lumBufIdx],
                gGpuBake.maxSrvLum, gGpuBake.minSrvLum
            };
            ctx->CSSetShaderResources(0, 5, srvs);
            ctx->CSSetUnorderedAccessViews(0, 1, &heightUav, initialCounts);
            ctx->CSSetConstantBuffers(0, 1, &gGpuBake.cbNormalize);
            ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
            ID3D11ShaderResourceView* nulls5[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
            ctx->CSSetShaderResources(0, 5, nulls5);
        }
    }

    // Restore CS state.
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &savedSrv0);
    ctx->CSSetShaderResources(1, 1, &savedSrv1);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav0, nullptr);
    ctx->CSSetUnorderedAccessViews(1, 1, &savedUav1, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &savedCb0);
    ctx->CSSetSamplers(0, 1, &savedSamp);
    if (savedCs)   savedCs->Release();
    if (savedSrv0) savedSrv0->Release();
    if (savedSrv1) savedSrv1->Release();
    if (savedUav0) savedUav0->Release();
    if (savedUav1) savedUav1->Release();
    if (savedCb0)  savedCb0->Release();
    if (savedSamp) savedSamp->Release();

    // Fill mips 1..N from the mip-0 writes. Standard 2x2 box downsampling
    // — effectively a low-pass cascade that DS sampling at higher mips
    // uses to suppress sub-tessellation spikes.
    ctx->GenerateMips(heightSrv);

    heightUav->Release();
    heightTex->Release(); // SRV holds a ref
    Log("TerrainTess: GPU bake complete (W=%u H=%u, 6 slices, mips generated)", W, H);
    return heightSrv;
}

// Bake-on-first-sighting: dispatch a decode CS to convert BC3 normalMaps to
// CPU-readable RGBA8, then run Frankot-Chellappa FFT integration on each
// slice in C++, then upload as an immutable R16_UNORM Texture2DArray.
// Same algorithm as the Python offline tool — no GPU integration involved.
//
// `diffuseSrv` is used by the GPU path to bake luminance-as-height alongside
// the FFT signal. Pass nullptr to disable the lum-mix entirely.
static ID3D11ShaderResourceView* BakeHeightArray(ID3D11Device* device,
                                                  ID3D11DeviceContext* ctx,
                                                  ID3D11ShaderResourceView* srcSrv,
                                                  ID3D11ShaderResourceView* diffuseSrv,
                                                  ID3D11Resource* srcResource)
{
    if (!gDecodeNormalsCs) return nullptr;

    auto it = gHeightArrays.find(srcResource);
    if (it != gHeightArrays.end()) return it->second.srv;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srcSrv->GetDesc(&srvDesc);
    if (srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY) return nullptr;
    if (srvDesc.Texture2DArray.ArraySize != 6) return nullptr;

    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(srcResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex)))
        return nullptr;
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTex->GetDesc(&srcDesc);
    srcTex->Release();

    // Step 1: dispatch decode CS to convert BC3 normals → R8G8B8A8.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = srcDesc.Width;
    td.Height = srcDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* decoded = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &decoded))) return nullptr;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    uavDesc.Texture2DArray.ArraySize = 6;
    ID3D11UnorderedAccessView* decodedUav = nullptr;
    if (FAILED(device->CreateUnorderedAccessView(decoded, &uavDesc, &decodedUav)))
    { decoded->Release(); return nullptr; }

    // Save CS state we'll trample.
    ID3D11ComputeShader*       savedCs    = nullptr;
    ID3D11ShaderResourceView*  savedSrv0  = nullptr;
    ID3D11UnorderedAccessView* savedUav   = nullptr;
    ID3D11SamplerState*        savedSamp  = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, 0);
    ctx->CSGetShaderResources(0, 1, &savedSrv0);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetSamplers(0, 1, &savedSamp);

    UINT initial = 0;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView*  nullSrv = nullptr;

    ctx->CSSetShader(gDecodeNormalsCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srcSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &decodedUav, &initial);
    ctx->CSSetSamplers(0, 1, &gHeightSampler);
    ctx->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 6);

    // Decode the matching diffuse array too, if available + lum-mix is on.
    // Reuses the same decode CS (BC3 → RGBA8) on diffuseSrv. We allocate
    // decodedDiffuse here and free it after the GPU bake.
    ID3D11Texture2D*           decodedDiffuse    = nullptr;
    ID3D11UnorderedAccessView* decodedDiffuseUav = nullptr;
    ID3D11ShaderResourceView*  decodedDiffuseSrv = nullptr;
    if (diffuseSrv && gBakeLumMix > 0.0f)
    {
        if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &decodedDiffuse)) &&
            SUCCEEDED(device->CreateUnorderedAccessView(decodedDiffuse, &uavDesc, &decodedDiffuseUav)))
        {
            ctx->CSSetShaderResources(0, 1, &diffuseSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &decodedDiffuseUav, &initial);
            ctx->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 6);

            D3D11_SHADER_RESOURCE_VIEW_DESC dvd = {};
            dvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            dvd.Texture2DArray.MipLevels = 1;
            dvd.Texture2DArray.ArraySize = 6;
            device->CreateShaderResourceView(decodedDiffuse, &dvd, &decodedDiffuseSrv);
        }
    }

    // Unbind UAV/SRV/sampler we set, restore previous.
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &savedSrv0);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
    ctx->CSSetSamplers(0, 1, &savedSamp);
    if (savedCs)   savedCs->Release();
    if (savedSrv0) savedSrv0->Release();
    if (savedUav)  savedUav->Release();
    if (savedSamp) savedSamp->Release();

    // Try the GPU FFT pipeline first. Returns the final heightArray SRV
    // directly. If it can't run (size != 2048, shader compile failure),
    // we fall through to the CPU path below.
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC dsvd = {};
        dsvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dsvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        dsvd.Texture2DArray.MipLevels = 1;
        dsvd.Texture2DArray.ArraySize = 6;
        ID3D11ShaderResourceView* decodedSrv = nullptr;
        if (SUCCEEDED(device->CreateShaderResourceView(decoded, &dsvd, &decodedSrv)))
        {
            ID3D11ShaderResourceView* gpuSrv = BakeHeightArrayGpu(
                device, ctx, decodedSrv, decodedDiffuseSrv, srcDesc.Width, srcDesc.Height);
            decodedSrv->Release();
            if (gpuSrv)
            {
                HeightArray ha;
                gpuSrv->GetResource((ID3D11Resource**)&ha.tex);
                ha.srv = gpuSrv;
                gHeightArrays[srcResource] = ha;
                decodedUav->Release();
                decoded->Release();
                if (decodedDiffuseSrv) decodedDiffuseSrv->Release();
                if (decodedDiffuseUav) decodedDiffuseUav->Release();
                if (decodedDiffuse)    decodedDiffuse->Release();
                return ha.srv;
            }
        }
        if (decodedDiffuseSrv) decodedDiffuseSrv->Release();
        if (decodedDiffuseUav) decodedDiffuseUav->Release();
        if (decodedDiffuse)    decodedDiffuse->Release();
    }

    // Step 2: copy decoded → staging for CPU readback.
    D3D11_TEXTURE2D_DESC stagingDesc = td;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
    { decodedUav->Release(); decoded->Release(); return nullptr; }
    ctx->CopyResource(staging, decoded);

    // Step 3: per-slice CPU FFT integration.
    UINT W = srcDesc.Width, H = srcDesc.Height;
    size_t N = (size_t)W * H;
    std::vector<float> p(N), q(N);
    std::vector<std::vector<uint16_t>> heightSlices(6);
    D3D11_SUBRESOURCE_DATA initData[6] = {};

    // Diagnostic: capture source RGBA8 for each slice, only on the very first
    // bake. Used by tools/verify_bake.py to compare against offline FFT output.
    static bool sCaptureFirst = true;
    bool captureNow = sCaptureFirst;
    sCaptureFirst = false;
    std::vector<std::vector<uint8_t>> sourceSlices;
    if (captureNow) sourceSlices.resize(6);

    for (UINT s = 0; s < 6; s++)
    {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(staging, s, D3D11_MAP_READ, 0, &m)))
        {
            heightSlices[s].assign(N, 32768);  // mid-gray fallback
            initData[s].pSysMem = heightSlices[s].data();
            initData[s].SysMemPitch = W * 2;
            continue;
        }
        if (captureNow)
        {
            sourceSlices[s].resize(N * 4);
            for (UINT y = 0; y < H; y++)
                memcpy(sourceSlices[s].data() + (size_t)y * W * 4,
                       (const char*)m.pData + (size_t)y * m.RowPitch, W * 4);
        }
        // Decode RGBA8 → gradient field. nx = R*2-1, ny = G*2-1, nz from
        // sqrt(1 - nx² - ny²). p = -nx/nz, q = -ny/nz.
        for (UINT y = 0; y < H; y++)
        {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (UINT x = 0; x < W; x++)
            {
                float nxf = row[x*4 + 0] / 127.5f - 1.0f;
                float nyf = row[x*4 + 1] / 127.5f - 1.0f;
                float k = 1.0f - nxf*nxf - nyf*nyf;
                if (k < 0.0025f) k = 0.0025f;
                float nzf = std::sqrt(k);
                size_t i = (size_t)y * W + x;
                p[i] = -nxf / nzf;
                q[i] = -nyf / nzf;
            }
        }
        ctx->Unmap(staging, s);

        FrankotChellappa(p.data(), q.data(), W, H, heightSlices[s]);
        initData[s].pSysMem = heightSlices[s].data();
        initData[s].SysMemPitch = W * 2;
    }

    staging->Release();
    decodedUav->Release();
    decoded->Release();

    // Diagnostic: write source RGBA8 to disk. Only happens once per session.
    if (captureNow)
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&BakeHeightArray, &self);
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(self, dllPath, MAX_PATH);
        std::string base(dllPath);
        auto slash = base.find_last_of("\\/");
        std::string srcPath = (slash != std::string::npos ? base.substr(0, slash + 1) : "")
                              + "baked_source_rgba.bin";
        FILE* fs = nullptr;
        fopen_s(&fs, srcPath.c_str(), "wb");
        if (fs)
        {
            uint32_t hdr[3] = { W, H, 6 };
            fwrite(hdr, 4, 3, fs);
            for (UINT s = 0; s < 6; s++)
                fwrite(sourceSlices[s].data(), 1, sourceSlices[s].size(), fs);
            fclose(fs);
            Log("TerrainTess: dumped source RGBA8 to %s", srcPath.c_str());
        }
    }

    // Step 4: create the immutable R16_UNORM Texture2DArray with our heights.
    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = W;
    dstDesc.Height = H;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 6;
    dstDesc.Format = DXGI_FORMAT_R16_UNORM;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_IMMUTABLE;
    dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HeightArray ha;
    if (FAILED(device->CreateTexture2D(&dstDesc, initData, &ha.tex)))
        return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC dstSrvDesc = {};
    dstSrvDesc.Format = DXGI_FORMAT_R16_UNORM;
    dstSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    dstSrvDesc.Texture2DArray.MipLevels = 1;
    dstSrvDesc.Texture2DArray.ArraySize = 6;
    if (FAILED(device->CreateShaderResourceView(ha.tex, &dstSrvDesc, &ha.srv)))
    {
        ha.tex->Release();
        return nullptr;
    }

    // Diagnostic dump of the first baked heightmap (uint16 raw, header u32x3).
    static bool sDumpedOnce = false;
    if (!sDumpedOnce)
    {
        sDumpedOnce = true;
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&BakeHeightArray, &self);
        char dllPath[MAX_PATH] = {};
        GetModuleFileNameA(self, dllPath, MAX_PATH);
        std::string path(dllPath);
        auto slash = path.find_last_of("\\/");
        path = (slash != std::string::npos ? path.substr(0, slash + 1) : "")
             + "baked_first_u16.bin";
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        if (f)
        {
            uint32_t hdr[3] = { W, H, 6 };
            fwrite(hdr, 4, 3, f);
            for (UINT s = 0; s < 6; s++)
                fwrite(heightSlices[s].data(), 2, N, f);
            fclose(f);
            Log("TerrainTess: dumped first CPU-baked heightmap to %s", path.c_str());
        }
    }

    gHeightArrays[srcResource] = ha;
    Log("TerrainTess: baked heightmaps via CPU FFT for normalMaps resource %p (%ux%u x6)",
        srcResource, W, H);
    return ha.srv;
}

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

    // Compile the two heightmap-bake compute shaders (row scan + col combine).
    auto compileCs = [&](const char* src, const char* entry, ID3D11ComputeShader** out)
    {
        ID3DBlob* errs = nullptr;
        ID3DBlob* blob = nullptr;
        HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                                entry, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                &blob, &errs);
        if (FAILED(hr) || !blob)
        {
            if (errs) Log("TerrainTess: CS compile failed (%s): %s", entry, (const char*)errs->GetBufferPointer());
            if (errs) errs->Release();
            return false;
        }
        if (errs) errs->Release();
        HRESULT hrC = device->CreateComputeShader(blob->GetBufferPointer(),
                                                  blob->GetBufferSize(),
                                                  nullptr, out);
        blob->Release();
        if (FAILED(hrC))
        {
            Log("TerrainTess: CreateComputeShader(%s) failed (0x%08X)", entry, hrC);
            return false;
        }
        return true;
    };
    compileCs(kDecodeNormalsCS, "main", &gDecodeNormalsCs);

    // GPU bake shaders. All entry points share one HLSL string.
    auto compileBakeCs = [&](const char* entry, ID3D11ComputeShader** out)
    {
        return compileCs(kHeightBakeCS, entry, out);
    };
    bool bakeCsOk = compileBakeCs("Extract",            &gExtractCs)
                 && compileBakeCs("FFT_1D",             &gFftCs)
                 && compileBakeCs("Integrate",          &gIntegrateCs)
                 && compileBakeCs("ReduceMaxAbs",       &gReduceCs)
                 && compileBakeCs("Normalize",          &gNormalizeCs)
                 && compileBakeCs("InitMax",            &gInitMaxCs)
                 && compileBakeCs("ExtractLumCentered", &gExtractLumCs)
                 && compileBakeCs("HighPass",            &gHighPassCs)
                 && compileBakeCs("ReduceMinMax",        &gReduceMinMaxCs);
    if (!bakeCsOk)
        Log("TerrainTess: GPU bake shaders failed to compile — will fall back to CPU FFT");

    // Allocate fixed-size 2048x2048 R32G32_FLOAT working textures + the
    // reduction max buffer + cbuffers. Reused across all slices.
    if (bakeCsOk)
    {
        const UINT N = 2048;
        gGpuBake.width  = N;
        gGpuBake.height = N;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = N;
        td.Height = N;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        bool bufOk = true;
        for (int i = 0; i < 3 && bufOk; i++)
        {
            if (FAILED(device->CreateTexture2D(&td, nullptr, &gGpuBake.tex[i])))
                bufOk = false;
            if (bufOk && FAILED(device->CreateShaderResourceView(
                gGpuBake.tex[i], nullptr, &gGpuBake.srv[i])))
                bufOk = false;
            if (bufOk && FAILED(device->CreateUnorderedAccessView(
                gGpuBake.tex[i], nullptr, &gGpuBake.uav[i])))
                bufOk = false;
        }

        if (bufOk)
        {
            D3D11_BUFFER_DESC mb = {};
            mb.ByteWidth = sizeof(uint32_t);
            mb.Usage = D3D11_USAGE_DEFAULT;
            mb.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            mb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            mb.StructureByteStride = sizeof(uint32_t);
            // Three reduction buffers: FFT max-abs, lum max, lum min.
            ID3D11Buffer** maxBufs[]   = { &gGpuBake.maxBuf, &gGpuBake.maxBufLum, &gGpuBake.minBufLum };
            ID3D11ShaderResourceView** maxSrvs[] = { &gGpuBake.maxSrv, &gGpuBake.maxSrvLum, &gGpuBake.minSrvLum };
            ID3D11UnorderedAccessView** maxUavs[] = { &gGpuBake.maxUav, &gGpuBake.maxUavLum, &gGpuBake.minUavLum };
            for (int i = 0; i < 3 && bufOk; i++)
            {
                if (FAILED(device->CreateBuffer(&mb, nullptr, maxBufs[i]))) { bufOk = false; break; }

                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = DXGI_FORMAT_UNKNOWN;
                sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                sd.Buffer.NumElements = 1;
                if (FAILED(device->CreateShaderResourceView(*maxBufs[i], &sd, maxSrvs[i])))
                { bufOk = false; break; }

                D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
                ud.Format = DXGI_FORMAT_UNKNOWN;
                ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                ud.Buffer.NumElements = 1;
                if (FAILED(device->CreateUnorderedAccessView(*maxBufs[i], &ud, maxUavs[i])))
                { bufOk = false; break; }
            }
        }

        if (bufOk)
        {
            // Five small dynamic cbuffers, one per shader. Each is 16 bytes.
            D3D11_BUFFER_DESC cb = {};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ID3D11Buffer** cbs[] = {
                &gGpuBake.cbExtract, &gGpuBake.cbFft, &gGpuBake.cbIntegrate,
                &gGpuBake.cbReduce, &gGpuBake.cbNormalize
            };
            for (auto* p : cbs)
                if (FAILED(device->CreateBuffer(&cb, nullptr, p))) { bufOk = false; break; }
        }

        if (!bufOk)
            Log("TerrainTess: GPU bake buffer alloc failed — falling back to CPU FFT");
    }

    // Linear-wrap sampler reused for compute (BC3 normal sampling) and DS
    // (heightmap array sampling). MaxLOD = ∞ so all mip levels are usable.
    {
        D3D11_SAMPLER_DESC ss = {};
        ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        ss.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        ss.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        ss.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&ss, &gHeightSampler);
    }

    // Dynamic cbuffer for tunable parameters (32 bytes = 8 floats).
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(Controls);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &gControlCb);
    }

    // Per-frame timestamp pool for the GPU-time perf metric. One disjoint
    // and N begin/end timestamp pairs per ring entry; ring depth = 3 to
    // avoid CPU stalls when reading back.
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

    Log("TerrainTess: passthrough HS+DS compiled and ready");
}

void Shutdown()
{
    if (gHs)            { gHs->Release();            gHs = nullptr; }
    if (gDs)            { gDs->Release();            gDs = nullptr; }
    if (gDecodeNormalsCs) { gDecodeNormalsCs->Release(); gDecodeNormalsCs = nullptr; }
    if (gExtractCs)     { gExtractCs->Release();     gExtractCs = nullptr; }
    if (gFftCs)         { gFftCs->Release();         gFftCs = nullptr; }
    if (gIntegrateCs)   { gIntegrateCs->Release();   gIntegrateCs = nullptr; }
    if (gReduceCs)      { gReduceCs->Release();      gReduceCs = nullptr; }
    if (gNormalizeCs)   { gNormalizeCs->Release();   gNormalizeCs = nullptr; }
    if (gInitMaxCs)     { gInitMaxCs->Release();     gInitMaxCs = nullptr; }
    if (gExtractLumCs)  { gExtractLumCs->Release();  gExtractLumCs = nullptr; }
    if (gHighPassCs)    { gHighPassCs->Release();    gHighPassCs = nullptr; }
    if (gReduceMinMaxCs){ gReduceMinMaxCs->Release();gReduceMinMaxCs = nullptr; }
    for (int i = 0; i < 3; i++)
    {
        if (gGpuBake.uav[i]) gGpuBake.uav[i]->Release();
        if (gGpuBake.srv[i]) gGpuBake.srv[i]->Release();
        if (gGpuBake.tex[i]) gGpuBake.tex[i]->Release();
        gGpuBake.uav[i] = nullptr;
        gGpuBake.srv[i] = nullptr;
        gGpuBake.tex[i] = nullptr;
    }
    if (gGpuBake.maxUav)      gGpuBake.maxUav->Release();
    if (gGpuBake.maxSrv)      gGpuBake.maxSrv->Release();
    if (gGpuBake.maxBuf)      gGpuBake.maxBuf->Release();
    if (gGpuBake.maxUavLum)   gGpuBake.maxUavLum->Release();
    if (gGpuBake.maxSrvLum)   gGpuBake.maxSrvLum->Release();
    if (gGpuBake.maxBufLum)   gGpuBake.maxBufLum->Release();
    if (gGpuBake.minUavLum)   gGpuBake.minUavLum->Release();
    if (gGpuBake.minSrvLum)   gGpuBake.minSrvLum->Release();
    if (gGpuBake.minBufLum)   gGpuBake.minBufLum->Release();
    if (gGpuBake.cbExtract)   gGpuBake.cbExtract->Release();
    if (gGpuBake.cbFft)       gGpuBake.cbFft->Release();
    if (gGpuBake.cbIntegrate) gGpuBake.cbIntegrate->Release();
    if (gGpuBake.cbReduce)    gGpuBake.cbReduce->Release();
    if (gGpuBake.cbNormalize) gGpuBake.cbNormalize->Release();
    gGpuBake = GpuBakeBuffers{};
    if (gHeightSampler) { gHeightSampler->Release(); gHeightSampler = nullptr; }
    if (gControlCb)     { gControlCb->Release();     gControlCb = nullptr; }
    if (gWireframeRs)   { gWireframeRs->Release();   gWireframeRs = nullptr; }
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
    for (auto& kv : gHeightArrays)
    {
        if (kv.second.tex)
        {
            if (kv.second.srv) kv.second.srv->Release();
            kv.second.tex->Release();
        }
    }
    gHeightArrays.clear();
    gSharedHeightArr = nullptr;
    gPsBytecode.clear();
    gPsBlendLevel.clear();
}

// Internal helpers ----------------------------------------------------------

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
        // Tessellate any main_fs draw (level >= 0). BLEND0 displaces using
        // its own heightArray. BLEND1+ chunks displace using the BASE layer's
        // heightArray (approximate — the BLEND1+ layer's own normals aren't
        // baked yet) but get the same subdivided topology so boundaries with
        // BLEND0 neighbors don't have T-junction cracks. simple_fs (distant
        // terrain, level=-1) has no scales and stays excluded.
        auto it = gPsBlendLevel.find(ps);
        int level = (it != gPsBlendLevel.end()) ? it->second : -1;
        ok = catOk && (level >= 0);
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    return ok;
}

// Read the source IB sub-range, generate equivalent list indices with
// alternating winding, create an immutable list IB. Caches by (src ptr, range).
// Returns the cached list IB and its index count via out params.
bool PrepareStripConversion(ID3D11DeviceContext* ctx,
                            ID3D11Buffer* origIB, DXGI_FORMAT origFormat,
                            UINT origByteOffset,
                            UINT stripIndexCount, UINT stripStartIndex,
                            ID3D11Buffer** outListIB, UINT* outListIndexCount)
{
    if (!origIB || stripIndexCount < 3) return false;
    if (origFormat != DXGI_FORMAT_R16_UINT && origFormat != DXGI_FORMAT_R32_UINT)
        return false;

    // No caching: Kenshi reuses the same IB pointer with MAP_DISCARD writes
    // for different chunks, so any cache keyed by (IB,start,count) returns
    // stale conversions when the IB content changes between draws (visible
    // as fan artifacts at chunk seams in wireframe). Always re-convert. The
    // caller is responsible for Releasing the returned listIB after the draw.

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return false;

    UINT indexSize = (origFormat == DXGI_FORMAT_R16_UINT) ? 2 : 4;

    // Staging copy so we can read the source IB on the CPU.
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

    // D3D11 TRIANGLESTRIP convention: even triangles emit (v0,v1,v2),
    // odd triangles emit (v1,v0,v2) so the rasterizer sees consistent
    // winding across the whole strip. We emit the same vertex order as
    // a TRIANGLELIST so the rasterizer produces identical triangles.
    //
    // Strips use degenerate triangles (two coincident vertices, zero area)
    // as row terminators — they're invisible in raster but tessellation +
    // displacement breaks the degeneracy and reveals them as long thin
    // spikes. Skip any triangle where two indices match.
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

    // Diagnostic — log a few source and list indices while data still mapped.
    {
        static int sLog = 0;
        if (sLog < 2)
        {
            sLog++;
            if (indexSize == 2)
            {
                const uint16_t* sp = (const uint16_t*)srcBase;
                const uint16_t* lp = (const uint16_t*)listData;
                Log("TerrainTess: src first 12: %u %u %u %u %u %u %u %u %u %u %u %u",
                    sp[0],sp[1],sp[2],sp[3],sp[4],sp[5],sp[6],sp[7],sp[8],sp[9],sp[10],sp[11]);
                Log("TerrainTess: list first 12: %u %u %u %u %u %u %u %u %u %u %u %u",
                    lp[0],lp[1],lp[2],lp[3],lp[4],lp[5],lp[6],lp[7],lp[8],lp[9],lp[10],lp[11]);
            }
            Log("TerrainTess: byteOff=%u startIdx=%u stripCnt=%u listCnt=%u fmt=%d",
                origByteOffset, stripStartIndex, stripIndexCount, listIndexCount, (int)origFormat);
        }
    }

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

void Begin(ID3D11DeviceContext* ctx)
{
    // Diagnostic: log the first few tessellated draws so we can confirm we're
    // actually subdividing terrain (and not silently skipping every draw).
    static int sCount = 0;
    if (sCount < 5)
    {
        sCount++;
        Log("TerrainTess: tessellating draw #%d", sCount);
    }

    // Look up (or bake) heightmap array matching the currently-bound
    // normalMaps Texture2DArray at PS slot 1.
    ID3D11ShaderResourceView* heightArrSrv = nullptr;
    {
        ID3D11ShaderResourceView* psSrv1 = nullptr;
        ctx->PSGetShaderResources(1, 1, &psSrv1);
        if (psSrv1)
        {
            ID3D11Resource* res = nullptr;
            psSrv1->GetResource(&res);
            if (res)
            {
                auto it = gHeightArrays.find(res);
                if (it != gHeightArrays.end())
                {
                    heightArrSrv = it->second.srv;
                }
                else if (gSharedHeightArr)
                {
                    // SHARED HEIGHTMAP: never bake more than one. All chunks
                    // bind the first heightArr we ever produced → no per-chunk
                    // variation, no seams.
                    heightArrSrv = gSharedHeightArr;
                    gHeightArrays[res] = HeightArray{nullptr, gSharedHeightArr};
                }
                else
                {
                    ID3D11Device* dev = nullptr;
                    ctx->GetDevice(&dev);
                    if (dev)
                    {
                        // Diffuse SRV at PS slot 0 — used by the bake to mix
                        // luminance-as-height when gBakeLumMix > 0. Optional;
                        // if null, bake falls back to FFT-only.
                        ID3D11ShaderResourceView* psSrv0 = nullptr;
                        ctx->PSGetShaderResources(0, 1, &psSrv0);
                        heightArrSrv = BakeHeightArray(dev, ctx, psSrv1, psSrv0, res);
                        if (psSrv0) psSrv0->Release();
                        dev->Release();
                        // Promote the very first bake to global shared.
                        if (heightArrSrv && !gSharedHeightArr) gSharedHeightArr = heightArrSrv;
                    }
                }
                res->Release();
            }
            psSrv1->Release();
        }
    }

    // Diagnostic: on first successful bake, dump the matching diffuse array
    // (PS slot 0) so verify_bake.py can show DIFFUSE | NORMAL | HEIGHT side
    // by side and we can compare displacement to visible texture.
    static bool sDumpedDiffuse = false;
    if (!sDumpedDiffuse && heightArrSrv)
    {
        sDumpedDiffuse = true;
        ID3D11ShaderResourceView* psSrv0 = nullptr;
        ctx->PSGetShaderResources(0, 1, &psSrv0);
        if (psSrv0)
        {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev)
            {
                DumpDecodedArray(dev, ctx, psSrv0, "baked_diffuse_rgba.bin");
                dev->Release();
            }
            psSrv0->Release();
        }
    }

    // Save current state — Kenshi/Ogre never sets HS/DS on its own, but be
    // defensive in case some other hook does.
    ctx->IAGetPrimitiveTopology(&gSaved.topo);
    ctx->HSGetShader(&gSaved.hs, nullptr, 0);
    ctx->DSGetShader(&gSaved.ds, nullptr, 0);
    ctx->HSGetConstantBuffers(0, 1, &gSaved.hsCb0);
    ctx->HSGetConstantBuffers(1, 1, &gSaved.hsCb1);
    ctx->DSGetConstantBuffers(0, 1, &gSaved.dsCb0);
    ctx->DSGetConstantBuffers(1, 1, &gSaved.dsCb1);
    ctx->DSGetShaderResources(0, 1, &gSaved.dsSrv0);
    ctx->DSGetShaderResources(1, 1, &gSaved.dsSrv1);
    ctx->DSGetShaderResources(2, 1, &gSaved.dsSrv2);
    ctx->DSGetShaderResources(3, 1, &gSaved.dsSrv3);
    ctx->DSGetShaderResources(4, 1, &gSaved.dsSrv4);
    ctx->DSGetShaderResources(5, 1, &gSaved.dsSrv5);
    ctx->DSGetShaderResources(6, 1, &gSaved.dsSrv6);
    ctx->DSGetShaderResources(7, 1, &gSaved.dsSrv7);
    ctx->DSGetShaderResources(8, 1, &gSaved.dsSrv8);
    ctx->DSGetShaderResources(9, 1, &gSaved.dsSrv9);
    ctx->DSGetShaderResources(10, 1, &gSaved.dsSrv10);
    ctx->DSGetShaderResources(11, 1, &gSaved.dsSrv11);
    ctx->DSGetSamplers(0, 1, &gSaved.dsSamp0);
    // Reused by the PS heightmap-debug-view patch.
    ctx->PSGetShaderResources(12, 1, &gSaved.psSrv12);
    ctx->PSGetConstantBuffers(1, 1, &gSaved.psCb1);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ctx->HSSetShader(gHs, nullptr, 0);
    ctx->DSSetShader(gDs, nullptr, 0);

    // Defensive: clear any GS that might be bound between DS and rasterizer.
    ID3D11GeometryShader* nullGs = nullptr;
    ctx->GSSetShader(nullGs, nullptr, 0);

    // Mirror PS cb0 (terrain $Globals: scalesA/B/C, slopeMin/Max/Blend, etc.)
    // to DS cb0 so DS can replicate the PS's slope/UV-scale logic when
    // sampling heightmap slices.
    {
        ID3D11Buffer* psCb0 = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &psCb0);
        ctx->DSSetConstantBuffers(0, 1, &psCb0);

        // Diagnostic: reflect every unique PS that hits this code path, so
        // we can find the one with the full terrain uniform set (scalesA/B/C).
        static std::unordered_map<ID3D11PixelShader*, bool> sLoggedPs;
        bool firstSighting = false;
        {
            ID3D11PixelShader* p = nullptr;
            ctx->PSGetShader(&p, nullptr, 0);
            if (p && sLoggedPs.find(p) == sLoggedPs.end())
            {
                sLoggedPs[p] = true;
                firstSighting = true;
            }
            if (p) p->Release();
        }
        if (firstSighting)
        {
            ID3D11PixelShader* curPs = nullptr;
            ctx->PSGetShader(&curPs, nullptr, 0);
            if (curPs)
            {
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
                                case D3D_SIT_CBUFFER:    tname = "CBUFFER"; break;
                                case D3D_SIT_TEXTURE:    tname = "TEXTURE"; break;
                                case D3D_SIT_SAMPLER:    tname = "SAMPLER"; break;
                                case D3D_SIT_STRUCTURED: tname = "STRUCTURED"; break;
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
                            UINT slot = 0xFFFFFFFF;
                            for (UINT j = 0; j < sd.BoundResources; j++)
                            {
                                D3D11_SHADER_INPUT_BIND_DESC ib = {};
                                refl->GetResourceBindingDesc(j, &ib);
                                if (ib.Type == D3D_SIT_CBUFFER && strcmp(ib.Name, bd.Name) == 0)
                                {
                                    slot = ib.BindPoint;
                                    break;
                                }
                            }
                            Log("  cb%u '%s' slot=%u size=%u vars=%u",
                                i, bd.Name, slot, bd.Size, bd.Variables);
                            for (UINT v = 0; v < bd.Variables; v++)
                            {
                                ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                                D3D11_SHADER_VARIABLE_DESC vd = {};
                                var->GetDesc(&vd);
                                Log("    %-30s offset=%4u size=%4u",
                                    vd.Name, vd.StartOffset, vd.Size);
                            }
                        }
                        refl->Release();
                    }
                }
                else
                {
                    Log("TerrainTess: PS=%p bytecode not captured (created before hook?)", curPs);
                }
                curPs->Release();
            }
        }

        if (psCb0) psCb0->Release();
    }

    // Bind matching heightmap array to DS slot 0; mirror PS overlayMap (slot 2)
    // to DS slot 1 so DS can replicate the PS's grass/dirt overlay blending.
    ctx->DSSetShaderResources(0, 1, &heightArrSrv);
    {
        ID3D11ShaderResourceView* psOverlay = nullptr;
        ctx->PSGetShaderResources(2, 1, &psOverlay);
        ctx->DSSetShaderResources(1, 1, &psOverlay);
        if (psOverlay) psOverlay->Release();
    }
    // Mirror PS textures used by the BLEND chain to DS slots 6-11. The DS
    // replicates the PS's exact computeBiome(BLEND0..3) over these.
    {
        ID3D11ShaderResourceView* psSrvs[6] = {};
        ctx->PSGetShaderResources(0,  1, &psSrvs[0]);  // diffuseMaps  → DS t6
        ctx->PSGetShaderResources(3,  1, &psSrvs[1]);  // colourMap    → DS t7
        ctx->PSGetShaderResources(6,  1, &psSrvs[2]);  // diffuseMaps1 → DS t8
        ctx->PSGetShaderResources(8,  1, &psSrvs[3]);  // diffuseMaps2 → DS t9
        ctx->PSGetShaderResources(10, 1, &psSrvs[4]);  // diffuseMaps3 → DS t10
        ctx->PSGetShaderResources(5,  1, &psSrvs[5]);  // blendMap     → DS t11
        ctx->DSSetShaderResources(6, 6, psSrvs);
        for (int i = 0; i < 6; i++) if (psSrvs[i]) psSrvs[i]->Release();
    }
    ctx->DSSetSamplers(0, 1, &gHeightSampler);

    // Populate per-PS BLEND# channel masks so the DS replica weights the
    // BLEND1/2/3 layers exactly as the PS does (PS uses #defines for channel
    // index, host captures these via OnTerrainPsCompiled and looks them up by
    // PS pointer here).
    {
        for (int i = 0; i < 4; i++)
        {
            gControls.blend1Mask[i] = 0.0f;
            gControls.blend2Mask[i] = 0.0f;
            gControls.blend3Mask[i] = 0.0f;
        }
        ID3D11PixelShader* curPsForMask = nullptr;
        ctx->PSGetShader(&curPsForMask, nullptr, 0);
        if (curPsForMask)
        {
            auto it = gPsBlendDefs.find(curPsForMask);
            if (it != gPsBlendDefs.end())
            {
                if (it->second.b1 >= 0 && it->second.b1 < 4) gControls.blend1Mask[it->second.b1] = 1.0f;
                if (it->second.b2 >= 0 && it->second.b2 < 4) gControls.blend2Mask[it->second.b2] = 1.0f;
                if (it->second.b3 >= 0 && it->second.b3 < 4) gControls.blend3Mask[it->second.b3] = 1.0f;
            }
            curPsForMask->Release();
        }
    }

    // === Cross-region neighbor binding ===
    // Identify this chunk's spatial neighbors via the cbuffer-snoop infrastructure
    // and bind their heightArrays to DS slots t2..t5 (-X, +X, -Z, +Z respectively).
    // Falls back gracefully to binding the PRIMARY heightArray when no snooped
    // data is available yet (first frame for this VS) or no neighbor exists at
    // a given direction. With own-as-fallback, the DS blend is a no-op there.
    ID3D11ShaderResourceView* nbrSrv[4] = { heightArrSrv, heightArrSrv,
                                            heightArrSrv, heightArrSrv };
    {
        ID3D11VertexShader* curVs = nullptr;
        ctx->VSGetShader(&curVs, nullptr, 0);
        ID3D11Buffer* curVsCb = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &curVsCb);
        if (curVs && curVsCb)
        {
            // Mark this CB for snooping on future Maps. First sighting only —
            // the insert is no-op on duplicate.
            TrackCbuffer(curVsCb);

            // Try to extract chunk bounds from snooped data (1-frame lag).
            float ox = 0, oz = 0, sx = 0, sz = 0;
            if (GetChunkBoundsFromVS(curVs, curVsCb, ox, oz, sx, sz))
            {
                // Push chunk origin (= worldMatrix translation) to shader cb
                // so debug + boundary fade anchor to the actual chunk grid.
                gControls.chunkOriginX = ox;
                gControls.chunkOriginZ = oz;

                // Quantize to grid by user-set chunk size. Falls back to
                // overlayData region size if the user hasn't tuned the slider
                // yet (better than nothing — at least region-level neighbors).
                float unit = gControls.chunkSizeWorld > 1e-3f
                                ? gControls.chunkSizeWorld
                                : (sx + sz) * 0.5f;
                if (unit < 1e-3f) unit = 32.0f;
                gChunkGridUnit = unit;
                int gx = (int)floorf(ox / unit + 0.5f);
                int gz = (int)floorf(oz / unit + 0.5f);

                // Record this chunk in the per-frame grid for future neighbor
                // lookups (other draws in the same frame can find this one).
                gChunkGrid[ChunkKey{gx, gz}] = heightArrSrv;

                // Look up the 4 axis-aligned neighbors. If a neighbor was
                // already drawn this frame, we have its heightArray. If
                // not, we keep the fallback (own heightArray = no blend).
                struct Off { int dx, dz; int slot; };
                Off offsets[4] = { {-1, 0, 0}, {+1, 0, 1}, {0, -1, 2}, {0, +1, 3} };
                for (int i = 0; i < 4; i++)
                {
                    auto it = gChunkGrid.find(ChunkKey{gx + offsets[i].dx,
                                                        gz + offsets[i].dz});
                    if (it != gChunkGrid.end() && it->second)
                        nbrSrv[offsets[i].slot] = it->second;
                }
            }
        }
        if (curVsCb) curVsCb->Release();
        if (curVs)   curVs->Release();
    }
    ctx->DSSetShaderResources(2, 4, nbrSrv);

    // Upload TessControl cbuffer NOW (after chunkBounds is set above).
    // Bind to HS/DS b1 and PS b1 (PS reads gDebugViewMode for the debug view).
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
        ctx->PSSetConstantBuffers(1, 1, &gControlCb);
    }

    // Bind same heightArray to PS slot 12 (used by the heightmap-debug-view PS patch).
    ctx->PSSetShaderResources(12, 1, &heightArrSrv);
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
    ctx->DSSetShaderResources(0, 1, &gSaved.dsSrv0);
    ctx->DSSetShaderResources(1, 1, &gSaved.dsSrv1);
    ctx->DSSetShaderResources(2, 1, &gSaved.dsSrv2);
    ctx->DSSetShaderResources(3, 1, &gSaved.dsSrv3);
    ctx->DSSetShaderResources(4, 1, &gSaved.dsSrv4);
    ctx->DSSetShaderResources(5, 1, &gSaved.dsSrv5);
    ctx->DSSetShaderResources(6, 1, &gSaved.dsSrv6);
    ctx->DSSetShaderResources(7, 1, &gSaved.dsSrv7);
    ctx->DSSetShaderResources(8, 1, &gSaved.dsSrv8);
    ctx->DSSetShaderResources(9, 1, &gSaved.dsSrv9);
    ctx->DSSetShaderResources(10, 1, &gSaved.dsSrv10);
    ctx->DSSetShaderResources(11, 1, &gSaved.dsSrv11);
    ctx->DSSetSamplers(0, 1, &gSaved.dsSamp0);
    ctx->PSSetShaderResources(12, 1, &gSaved.psSrv12);
    ctx->PSSetConstantBuffers(1, 1, &gSaved.psCb1);

    if (gSaved.hs)      { gSaved.hs->Release();      gSaved.hs = nullptr; }
    if (gSaved.ds)      { gSaved.ds->Release();      gSaved.ds = nullptr; }
    if (gSaved.hsCb0)   { gSaved.hsCb0->Release();   gSaved.hsCb0 = nullptr; }
    if (gSaved.hsCb1)   { gSaved.hsCb1->Release();   gSaved.hsCb1 = nullptr; }
    if (gSaved.dsCb0)   { gSaved.dsCb0->Release();   gSaved.dsCb0 = nullptr; }
    if (gSaved.dsCb1)   { gSaved.dsCb1->Release();   gSaved.dsCb1 = nullptr; }
    if (gSaved.dsSrv0)  { gSaved.dsSrv0->Release();  gSaved.dsSrv0 = nullptr; }
    if (gSaved.dsSrv1)  { gSaved.dsSrv1->Release();  gSaved.dsSrv1 = nullptr; }
    if (gSaved.dsSrv2)  { gSaved.dsSrv2->Release();  gSaved.dsSrv2 = nullptr; }
    if (gSaved.dsSrv3)  { gSaved.dsSrv3->Release();  gSaved.dsSrv3 = nullptr; }
    if (gSaved.dsSrv4)  { gSaved.dsSrv4->Release();  gSaved.dsSrv4 = nullptr; }
    if (gSaved.dsSrv5)  { gSaved.dsSrv5->Release();  gSaved.dsSrv5 = nullptr; }
    if (gSaved.dsSrv6)  { gSaved.dsSrv6->Release();  gSaved.dsSrv6 = nullptr; }
    if (gSaved.dsSrv7)  { gSaved.dsSrv7->Release();  gSaved.dsSrv7 = nullptr; }
    if (gSaved.dsSrv8)  { gSaved.dsSrv8->Release();  gSaved.dsSrv8 = nullptr; }
    if (gSaved.dsSrv9)  { gSaved.dsSrv9->Release();  gSaved.dsSrv9 = nullptr; }
    if (gSaved.dsSrv10) { gSaved.dsSrv10->Release(); gSaved.dsSrv10 = nullptr; }
    if (gSaved.dsSrv11) { gSaved.dsSrv11->Release(); gSaved.dsSrv11 = nullptr; }
    if (gSaved.dsSamp0) { gSaved.dsSamp0->Release(); gSaved.dsSamp0 = nullptr; }
    if (gSaved.psSrv12) { gSaved.psSrv12->Release(); gSaved.psSrv12 = nullptr; }
    if (gSaved.psCb1)   { gSaved.psCb1->Release();   gSaved.psCb1 = nullptr; }
    gSaved.topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

// Bracket the tess draw with timestamp queries from the current frame's
// timer pool. If pool is full or queries weren't allocated, the draw runs
// untimed (still rendered correctly).
namespace {
bool TimerBeginDraw(ID3D11DeviceContext* ctx)
{
    auto& tf = gTimerFrames[gTimerCur];
    if (!tf.disjoint || tf.used >= tf.tsBegin.size()) return false;
    if (tf.used == 0) ctx->Begin(tf.disjoint);   // first draw of frame opens disjoint
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

    // Wireframe mode 2: vanilla strip draw with wireframe rasterizer (no tess,
    // no IB conversion). Shows Kenshi's untouched terrain mesh.
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

    // Wireframe mode 3: strip→list conversion + TRIANGLELIST topology, NO HS/DS.
    // Isolates the IB conversion from the tess pipeline so we can verify the
    // converted triangles match what strip rasterization would produce.
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

    // Mode 1 = tess wireframe: same tess path but with wireframe rasterizer.
    // Begin()/End() save and restore RS, so we override AFTER Begin() and let
    // End() restore.
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
        bool ok = PrepareStripConversion(ctx, origIB, origFormat, origOffset,
                                         indexCount, startIndex, &listIB, &listIC);
        if (!ok)
        {
            origIB->Release();
            return false;
        }

        // Main tess pass: use converted list IB.
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
