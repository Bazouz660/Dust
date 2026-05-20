#include "TerrainTess.h"
#include "ShaderDatabase.h"
#include "D3D11Hook.h"
#include "DustLog.h"

#include <tracy/Tracy.hpp>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <algorithm>

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

// Shared shader code used by BOTH HS and DS — struct VsOut/HsConst, texture
// bindings, cbuffers, and the ComputeDisplacementH helper. The HS uses the
// helper to compute h at each patch corner (gSpikeCap > 0 only); the DS uses
// the helper at the interpolated tess-vertex position. Linking corners → DS
// allows a soft cap on h_actual − h_interp, which flattens spatially-contained
// spikes (cones/pyramids) while leaving spatially-extended ridges (dunes) alone.
static const char* kCommonShader = R"HLSL(
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

// Live PS textures mirrored per draw onto both HS and DS slots — bound by
// Begin() before each tess draw. Slot numbering is local to HS/DS (not PS).
Texture2D      overlayMap   : register(t0);
Texture2DArray diffuseMaps  : register(t1);
Texture2D      colourMap    : register(t2);
Texture2DArray diffuseMaps1 : register(t3);
Texture2DArray diffuseMaps2 : register(t4);
Texture2DArray diffuseMaps3 : register(t5);
Texture2D      blendMap     : register(t6);
SamplerState   linearWrap   : register(s0);

// Mirror of PS $Globals cbuffer (terrain.hlsl). Offsets verified by reflection.
cbuffer PsTerrainCb : register(b0)
{
    float4 gPsViewport      : packoffset(c0);
    float4 gPsFarClipCamPos : packoffset(c1);
    float4 gPsWaterWetness  : packoffset(c2);
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
    float4 gPsScalesA1      : packoffset(c14);
    float4 gPsScalesB1      : packoffset(c15);
    float4 gPsScalesC1      : packoffset(c16);
    float4 gPsSlopeMin1     : packoffset(c17);
    float4 gPsSlopeMax1     : packoffset(c18);
    float4 gPsSlopeBlend1   : packoffset(c19);
    float4 gPsOverlayMult1  : packoffset(c20);
    float4 gPsScalesA2      : packoffset(c24);
    float4 gPsScalesB2      : packoffset(c25);
    float4 gPsScalesC2      : packoffset(c26);
    float4 gPsSlopeMin2     : packoffset(c27);
    float4 gPsSlopeMax2     : packoffset(c28);
    float4 gPsSlopeBlend2   : packoffset(c29);
    float4 gPsOverlayMult2  : packoffset(c30);
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
    float gSharpMip;
    float gScale;
    float gHfWeight;
    // LF-aware spike cap. Computes h with both the full pipeline and an
    // LF-only pipeline (slice_lo). Soft-caps the difference (HF/MF excess),
    // leaving the LF surface untouched. 0 = off.
    float gSpikeCap;
    // Per-slice mip offsets. Bands at offset=0: hi=K..K+1, hm=K+1..K+2,
    // mid=K+2..K+4, lo=K+4..K+8. Raising one offset blurs that band only.
    float gSmoothHi;
    float gSmoothHiMid;
    float gSmoothMid;
    float gSmoothLo;
    // Per-tap distance cutoffs. Past gFarHi we skip slice_hi + slice_hm;
    // past gFarMid we additionally skip slice_mid. slice_lo is always
    // sampled (until the amp-fade early-out runs above). Values are in
    // clip-space w (≈ view-space depth).
    float gFarHi;
    float gFarMid;
    float gSkipDistance;  // CPU-only — declared here just to match C++ layout
    float _farPad1;
    float4 gBlend1Mask;
    float4 gBlend2Mask;
    float4 gBlend3Mask;
};

float Lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// One BLEND layer's albedo. Mirrors computeBiome from terrainfp4.hlsl.
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

// PS-visible luminance at a given mip via the BLEND0+1+2+3 chain.
//
// The caller (ComputeDisplacementH) hoists the mip-0 texture samples
// (overlayMap / colourMap / blendMap) and passes them in — those samples are
// mip-independent, so the previous version was re-doing them 8 times for
// nothing. The slope + smoothstep ALU stays inline; the HLSL compiler CSEs
// it across the inlined call sites since none of the inputs change.
//
//   omap   : .rgba from overlayMap mip 0, with .r already maxed against .g
//   colour : .rgb  from colourMap  mip 0, already multiplied by 1.2
//   bw     : .rgba from blendMap   mip 0 (raw — w1/2/3 dots done here)
float ComputePsLum(float3 tex0, float2 cliffBlend, float4 mapCoords,
                   float3 normal, float3 texV, float mipLevel,
                   float4 omap, float3 colour, float4 bw)
{
    float slope = 1.0 - normalize(normal).y;

    float4 w0 = smoothstep(gPsSlopeMin - gPsSlopeBlend, gPsSlopeMin, slope)
              * smoothstep(gPsSlopeMax + gPsSlopeBlend, gPsSlopeMax, slope);
    float3 a = ComputeBiomeAlbedo(diffuseMaps, tex0, cliffBlend, w0, omap, colour,
                                  gPsScalesA, gPsScalesB, gPsScalesC, gPsOverlayMult, mipLevel);
    a *= gPsBrightnessFix.x;

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

// Displacement pipeline: returns BOTH the full-pipeline h (slice_lo + mid +
// hi, weighted) and an LF-only h (slice_lo alone). The two outputs share the
// expensive 4-mip PSLum sample, so they cost ~the same as one call.
//
// .x = h_full (the displacement we'd normally write)
// .y = h_lf   (the broad LF-only displacement = "dune surface")
//
// DS uses h_lf as a baseline and soft-caps (h_full − h_lf), which is the HF/MF
// "excess" sitting above the broad surface. Pure-LF features (dunes) have
// h_full ≈ h_lf → no excess → no cap. HF-rich features (rocks) have a big
// excess → it gets capped to a small magnitude.
float2 ComputeDisplacementH(float3 vnormal, float3 vtex0, float4 vtex1,
                            float2 vuvblend, float3 vtexV, float3 vworldPos)
{
    // True world-space distance from camera to this vertex. Replaces the
    // old vposW-based fade (which never fired because Kenshi clip-space w
    // is negative for visible geometry — see HS for details).
    float vposDist = length(vworldPos - gPsFarClipCamPos.yzw);

    float ampScale = lerp(1.0, 1.0 - smoothstep(gAmpFadeStart, gAmpFadeEnd, vposDist), gAmpFadeEnabled);
    float ampScaledTotal = gAmplitude * ampScale;
    if (ampScaledTotal < 1e-4) return float2(0.0, 0.0);

    // Hoist the three mip-0 samples (overlayMap / colourMap / blendMap) out of
    // ComputePsLum — they don't depend on the per-tap mipLevel, and the FXC
    // compiler doesn't CSE texture samples across inlined call sites, so
    // re-doing them 8 times was real work. 24 mip-0 samples → 3.
    float4 omap   = overlayMap.SampleLevel(linearWrap, vtex1.xy, 0).rgba;
    omap.r        = max(omap.r, omap.g);
    float3 colour = colourMap.SampleLevel(linearWrap, vtex1.xy, 0).rgb * 1.2;
    float4 bw     = blendMap.SampleLevel(linearWrap, vtex1.zw, 0);

    // Per-slice tap pairs with independent mip offsets. Default offsets = 0
    // give contiguous bands at K..K+1, K+1..K+2, K+2..K+4, K+4..K+8. Nonzero
    // offsets blur a specific band without touching the others.
    //
    // Distance LOD: HF bands aren't visible at distance, so the [branch]es
    // below skip those taps past gFarHi / gFarMid. slice_lo is always sampled
    // (it's the LF dune surface, the dominant signal at distance — and amp
    // fade kills it eventually via the early-out above).
    float vL0  = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 4.0 + gSmoothLo,    omap, colour, bw);
    float vL1  = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 8.0 + gSmoothLo,    omap, colour, bw);
    float slice_lo  = vL0 - vL1;

    float slice_mid = 0.0;
    float slice_hm  = 0.0;
    float slice_hi  = 0.0;

    [branch] if (vposDist < gFarMid)
    {
        float vM0 = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 2.0 + gSmoothMid, omap, colour, bw);
        float vM1 = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 4.0 + gSmoothMid, omap, colour, bw);
        slice_mid = vM0 - vM1;

        [branch] if (vposDist < gFarHi)
        {
            float vHM0 = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 1.0 + gSmoothHiMid, omap, colour, bw);
            float vHM1 = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 2.0 + gSmoothHiMid, omap, colour, bw);
            slice_hm = vHM0 - vHM1;

            float vH0  = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip       + gSmoothHi, omap, colour, bw);
            float vH1  = ComputePsLum(vtex0, vuvblend, vtex1, vnormal, vtexV, gSharpMip + 1.0 + gSmoothHi, omap, colour, bw);
            slice_hi = vH0 - vH1;
        }
    }

    // refLum was a 9th colourMap sample. Reuse the hoisted one.
    float refLum = max(Lum(colour), 0.05);

    float threshold = max(gScale, 1e-5) * 0.5;
    float keep      = smoothstep(threshold, threshold * 2.0, abs(slice_mid));

    float w_md = gHfWeight;
    float w_hf = gHfWeight * gHfWeight * keep;  // shared by slice_hi + slice_hm
    float bp_full = (slice_lo + slice_mid * w_md + (slice_hi + slice_hm) * w_hf) / refLum;
    float bp_lf   = slice_lo / refLum;

    float shaped_full = bp_full / (abs(bp_full) + max(gScale, 1e-5));
    float shaped_lf   = bp_lf   / (abs(bp_lf)   + max(gScale, 1e-5));

    float h_full = (shaped_full + gDisplacementBias) * ampScaledTotal;
    float h_lf   = (shaped_lf   + gDisplacementBias) * ampScaledTotal;
    return float2(h_full, h_lf);
}
)HLSL";

// HS entry: passthrough control points. HsConstFn just computes per-edge tess
// factors from the view-space w. All displacement work is in the DS now.
static const char* kHullEntry = R"HLSL(
HsConst HsConstFn(InputPatch<VsOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    // World-space distance from camera to each patch corner. Kenshi's
    // clip-space w is negative for visible verts, so the old pos.w-based
    // fade never engaged; using true world distance against cameraPos
    // (smuggled in via the PS cb0 mirror at gPsFarClipCamPos.yzw) makes
    // the factor fade actually fire and adapt with distance.
    float3 cam = gPsFarClipCamPos.yzw;
    float d0 = length(patch[0].worldPos - cam);
    float d1 = length(patch[1].worldPos - cam);
    float d2 = length(patch[2].worldPos - cam);
    float wEdge0 = (d1 + d2) * 0.5;
    float wEdge1 = (d0 + d2) * 0.5;
    float wEdge2 = (d0 + d1) * 0.5;

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

// DS entry: per-tess-vertex displacement. Computes h_full and h_lf via the
// shared helper, then soft-caps (h_full − h_lf) — the HF/MF excess above the
// broad LF surface. Pure-LF features (dunes) have h_full ≈ h_lf → 0 excess →
// no cap. HF features (rocks) have a large excess → capped → only h_lf
// (broad LF shape) plus a small clamped HF residual.
static const char* kDomainEntry = R"HLSL(
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

    // Full and LF-only h, sharing the 4-mip PSLum chain.
    float2 hPair = ComputeDisplacementH(o.normal, o.tex0, o.tex1, o.uvblend, o.texV.xyz, o.worldPos);
    float h_full = hPair.x;
    float h_lf   = hPair.y;

    // LF-aware spike cap: keep h_lf intact, soft-cap only the HF/MF excess.
    float h;
    if (gSpikeCap > 1e-5)
    {
        float excess        = h_full - h_lf;
        float excess_capped = excess / (1.0 + gSpikeCap * abs(excess));
        h = h_lf + excess_capped;
    }
    else
    {
        h = h_full;
    }

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
// Depth state for the blood-on-terrain pass. The blood material's DSS uses
// depth_func=equal, which fails at grazing angles where tess-introduced
// FP precision loss makes the re-rendered blood mesh's depth drift from
// the GBuffer terrain depth by < 1 ulp. LESS_EQUAL + depth_write=off keeps
// the geometry correctly occluded by characters/objects in front of the
// terrain while being tolerant of those sub-ulp drifts.
ID3D11DepthStencilState*  gBloodDss      = nullptr;
Controls                  gControls;

// GPU strip→list compute shader — converts TRIANGLESTRIP indices to
// TRIANGLELIST directly on the GPU via a compute dispatch. Replaces the
// CPU-side IB shadow + memcpy that was costing ~1.3ms/frame in
// OnContextUnmap.ib + OnContextUnmap.staging. Now there is zero CPU-side
// IB shadowing; the compute shader reads from the game's IB via
// CopySubresourceRegion → typed SRV and writes to a UAV-enabled IB +
// indirect draw args buffer. DrawIndexedInstancedIndirect consumes the
// result with no CPU readback of the index count.
static const char kStripToListCS[] = R"(
cbuffer Params : register(b0) { uint stripCount; uint pad0, pad1, pad2; };
Buffer<uint>        stripIB : register(t0);
RWBuffer<uint>      listIB  : register(u0);
RWByteAddressBuffer args    : register(u1);
[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint i = dtid.x;
    if (i + 2 >= stripCount) return;
    uint a = stripIB[i], b = stripIB[i+1], c = stripIB[i+2];
    if (a == b || b == c || a == c) return;
    if (i & 1) { uint t = a; a = b; b = t; }
    uint base;
    args.InterlockedAdd(0, 3u, base);
    listIB[base] = a; listIB[base+1] = b; listIB[base+2] = c;
}
)";

ID3D11ComputeShader*         gStripCS       = nullptr;
ID3D11Buffer*                gStripCB       = nullptr;
ID3D11Buffer*                gStripSrc      = nullptr;
UINT                         gStripSrcCap   = 0;
ID3D11ShaderResourceView*    gStripSrcSRV   = nullptr;
DXGI_FORMAT                  gStripSrcFmt   = DXGI_FORMAT_UNKNOWN;
ID3D11Buffer*                gStripDst      = nullptr;
UINT                         gStripDstCap   = 0;
ID3D11UnorderedAccessView*   gStripDstUAV   = nullptr;
DXGI_FORMAT                  gStripDstFmt   = DXGI_FORMAT_UNKNOWN;
ID3D11Buffer*                gStripArgs     = nullptr;
ID3D11UnorderedAccessView*   gStripArgsUAV  = nullptr;

// Captured PS bytecode by PS pointer (kept for reflection logging on first
// sighting of each unique PS — useful when debugging cb0 layout changes).
std::unordered_map<ID3D11PixelShader*, std::vector<uint8_t>> gPsBytecode;

// Per-PS blend level: -1 = simple_fs (distant terrain, no scales — exclude),
// 0..3 = main_fs with that many BLEND# extension layers. We tessellate level
// >= 0 only.
std::unordered_map<ID3D11PixelShader*, int> gPsBlendLevel;

// CPU-side per-chunk distance check. Routing a draw through HS+DS has fixed
// GPU pipeline overhead on most drivers — measurably ~5ms in busy scenes —
// regardless of how trivial the HS/DS work itself is. So once we know a
// chunk's origin is far past where displacement matters, we want to skip
// the entire HS/DS routing and just submit the original draw.
//
// To know "is this chunk far?" we need the chunk's clip-space position,
// which lives in the VS's worldViewProjMatrix cbuffer. We can't Map(READ)
// that buffer (it's CPU-write-only DYNAMIC), so we instead shadow its
// contents via the existing Map/Unmap hooks (CSMIntercept-style pattern).
//
// At first sighting of a terrain VS, reflection finds the matrix's byte
// offset. At each terrain DrawIndexed, we read the matrix from the shadow,
// compute the clip-space w of local (0,0,0,1) — the chunk's origin — and
// compare against ampFadeEnd + safety margin.
namespace {

// One offset per app (assumes all terrain VS variants share cbuffer layout —
// they're compiled from the same source). -1 = not yet found.
int           gTerrainWvpOffset  = -1;
// Cbuffer SLOT (register bN) the matrix lives in. Usually 0.
UINT          gTerrainWvpCbSlot  = 0;
// worldOffset uniform offset in $Params. Kenshi terrain stores vertex
// positions in chunk-LOCAL space and the VS adds `worldOffset` (a vec3
// uniform that varies per chunk) to produce world coords. There are
// multiple terrain VS variants with DIFFERENT cbuffer layouts — variant 1
// has worldOffset at byte 128, variant 2 has `morph` (a single float) at
// byte 128. So the offset must be tracked per-VS, not globally; -1 means
// "this VS variant has no worldOffset uniform (or hasn't been seen yet)".
std::unordered_map<ID3D11VertexShader*, int> gPerVsOffsetOffset;
// cameraPos uniform offset per VS variant. We use this to convert the
// chunk's world position (read from VB[0]) into a camera-relative distance.
std::unordered_map<ID3D11VertexShader*, int> gPerVsCameraPosOffset;
// worldMatrix offset within the shadowed cb. Its translation column is the
// camera-related world transform — invariant per draw within a frame, so
// chunkPos - worldMatrix.translation gives a true 3D distance that's
// invariant under camera rotation (unlike clip-space distance, which the
// projection matrix scales anisotropically across axes).
int gTerrainWorldMatrixOffset = -1;
// PS-side cameraPos uniform. Kenshi's terrain PS has a `cameraPos` vec3
// in its $Globals cbuffer used for POM/etc. — this is the actual camera
// world position (unlike the VS-side cameraPos which reads as zeros).
int gTerrainPsCameraPosOffset = -1;

// Latest camera world position observed during a terrain draw. Updated
// every frame the CPU-side skip-check runs; exposed via
// TerrainTess::GetCameraPos and the host API.
float gLastCameraPos[3] = { 0, 0, 0 };
bool  gHaveCameraPos    = false;

// Per-frame cached cb extracts. cameraPos + worldMatrix.translation are
// frame constants in Kenshi's camera-relative rendering, so we extract
// them once per frame instead of per terrain draw.
//
// Source path: CopySubresourceRegion(staging[write], psCb0/vsCb) +
// Map(READ) staging[read]. Double-buffered ring so Map(READ) on the
// previous slot never stalls (GPU drained it a full frame ago). One
// frame stale data is fine for distance culling (camera moves <1 unit/
// frame at 60fps; skipDistance is thousands of units).
//
// This replaces the previous gCbShadow-backed read, decoupling the tess
// metric from the per-Map shadow path entirely — when blood is inactive,
// PS/VS cb0 buffers no longer need to be tracked at all.
static uint32_t gFrameSkipDataFrame = 0xFFFFFFFFu;
static float    gFrameCamX = 0, gFrameCamY = 0, gFrameCamZ = 0;
static float    gFrameShiftX = 0, gFrameShiftY = 0, gFrameShiftZ = 0;
static bool     gFrameHaveCam = false, gFrameHaveShift = false;
constexpr int kCbStagingSlots = 2;
static ID3D11Buffer* gCamStaging[kCbStagingSlots]   = {};
static ID3D11Buffer* gShiftStaging[kCbStagingSlots] = {};
static int  gCbStagingWriteSlot = 0;
static bool gCbStagingPrimed    = false;

// VB[0] readback cache. Each terrain chunk has its own VB; we cache the
// first vertex's position by VB pointer so subsequent draws are pure
// hashmap reads. Capture-at-create handles VBs uploaded with initial
// data immediately; a budget-throttled CopyResource+Map(READ) fallback
// at draw time handles the rest (one stall per VB, then cached forever).
struct VbPos { float x, y, z; };
std::unordered_map<ID3D11Buffer*, VbPos> gVbPosCache;
ID3D11Buffer*                           gVbPosStaging   = nullptr;
int                                     gVbCaptureBudget = 0;
constexpr int                           kVbCaptureBudgetPerFrame = 8;

// Buffers that have been bound as VS cb[gTerrainWvpCbSlot] during a terrain
// draw. The Map/Unmap hook only shadows buffers in this set.
std::unordered_set<ID3D11Buffer*>          gTrackedCbs;
// Shadow store: latest CPU-visible content of each tracked buffer.
std::unordered_map<ID3D11Buffer*, std::vector<uint8_t>> gCbShadow;
// Mapped pointer between Map and Unmap calls (handed across by D3D11Hook).
std::unordered_map<ID3D11Buffer*, void*>   gCbPending;
// Single mutex covers all three above. Map/Unmap and DrawIndexed are on the
// same immediate-context thread in normal Kenshi operation, but the mutex
// is cheap and keeps things sound if that ever changes.
std::mutex                                 gCbMutex;

} // anon


// Captured BLEND1/2/3 channel-index #defines (0..3 = R/G/B/A) per PS.
// OnTerrainPsCompiled stashes by bytecode hash (called from the D3DCompile
// hook). OnPixelShaderCreated re-hashes the bytecode and resolves the entry
// into a pre-expanded float4 mask triplet for direct per-draw memcpy.
struct PsBlendDefines { int b1 = -1, b2 = -1, b3 = -1; };
static std::unordered_map<uint64_t, PsBlendDefines>            gBlendDefsByHash;

// Per-PS expanded masks. Each entry is exactly the 3×float4 tail of
// Controls (blend1Mask/blend2Mask/blend3Mask), so Begin() can memcpy in one
// shot. All-zero = the corresponding BLEND# define was absent → layer inactive
// at this PS, DS chain skips it.
struct PsBlendMasks { float m[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }; };
static std::unordered_map<ID3D11PixelShader*, PsBlendMasks>    gPsBlendMasks;

// Per-VB cache of the terrain-pass resources (PS-side SRVs + PS cb0 + PS
// pointer). Snapshot at the start of each successful tessellated terrain
// draw; used to re-bind those resources to HS/DS during the blood-on-terrain
// pass (which uses the same VB+IB but binds completely different PS slots).
//
// SRVs and cb0 are AddRef'd on capture and Released on overwrite/shutdown.
// terrainPs is a weak pointer used only as a key into gPsBlendMasks; the PS
// itself may outlive the VB, that's fine.
// Lock-free pre-filter for tracked buffers. Storage is declared in the
// TerrainTess::detail namespace at file scope below (outside the anonymous
// namespace) so the inline IsResourceTracked in the header can reach it.
inline void BloomAdd(void* p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 16;
    x *= 0x85ebca6bULL;
    x ^= x >> 13;
    uint32_t h = (uint32_t)(x & 65535);  // matches IsResourceTracked mask
    ::TerrainTess::detail::gTrackedBloom[h >> 6].fetch_or(
        uint64_t(1) << (h & 63), std::memory_order_relaxed);
}

inline bool BloomTest(void* p) { return IsResourceTracked(p); }

struct TerrainVbCtx
{
    ID3D11ShaderResourceView* srvs[7] = { nullptr };
    // PS and VS cb0 are DYNAMIC and Kenshi re-Maps them per pass. Holding the
    // buffer POINTER and re-binding later reads whatever content the engine
    // wrote LAST — typically the blood pass's $Globals (different layout) by
    // the time we draw, which scrambles cameraPos and scalesA. Result: HS
    // tess factors hop frame-to-frame, depth drifts, blood flickers.
    //
    // Instead we snapshot the CONTENT at capture time (via gCbShadow, which
    // the Map/Unmap hooks keep CPU-side up to date) and re-upload it to our
    // own pooled DYNAMIC buffer at blood-draw time.
    std::vector<uint8_t>      psCb0Data;
    std::vector<uint8_t>      vsCb0Data;
    UINT                      vsCb0Slot = 0;
    ID3D11PixelShader*        terrainPs = nullptr;
    // Last frame this entry was (re)captured. Blood pass only uses entries
    // captured THIS frame — if the chunk was tessellated last frame but
    // skipped this frame (e.g., camera moved out), the cached resources
    // describe displacement that no longer matches the current GBuffer
    // depth → applying them to blood would re-introduce the mismatch.
    uint32_t                  frame   = 0;
};
static std::unordered_map<ID3D11Buffer*, TerrainVbCtx>         gVbTerrainCtx;
// Incremented at the start of every frame (see OnFrameEnd, which the host
// calls at the front of each frame's work despite its name).
static uint32_t                                                gFrameNumber = 0;
// Pooled DYNAMIC cbuffers we re-upload terrain-time cb0 snapshots into
// before each blood draw. Sized lazily to the largest content seen. One
// buffer per slot since blood draws are serial on the immediate context.
static ID3D11Buffer*                                           gBloodPsCb0Pool  = nullptr;
static UINT                                                    gBloodPsCb0PoolSize = 0;
static ID3D11Buffer*                                           gBloodVsCb0Pool  = nullptr;
static UINT                                                    gBloodVsCb0PoolSize = 0;
// Adaptive blood capture: snapshotting per-VB resources per terrain draw
// costs ~10 COM calls + a mutex + a memcpy each, paid for every chunk in
// every frame. Most frames have no blood on terrain → the work is wasted.
//
// Strategy: only capture this frame if blood actually drew in the previous
// frame. The first blood frame after a long blood-free stretch can't tess
// (no cache yet), but the next frame onward works. For active combat the
// cache stays warm; for exploration scenes capture is fully skipped.
static bool                                                    gBloodDrewLastFrame = false;
static bool                                                    gBloodDrewThisFrame = false;

// Snapshot the seven SRVs + cb0 + ps pointer that the terrain GBuffer pass
// uses for `vb`. Called from Begin() when running a terrain (not blood) draw.
// AddRefs each captured resource; releases anything stored under that key.
static void CaptureTerrainVbCtx(ID3D11DeviceContext* ctx,
                                ID3D11Buffer* vb,
                                ID3D11PixelShader* ps)
{
    ZoneScoped;
    if (!ctx || !vb) return;
    auto& slot = gVbTerrainCtx[vb];
    for (int i = 0; i < 7; i++)
        if (slot.srvs[i]) { slot.srvs[i]->Release(); slot.srvs[i] = nullptr; }
    // Match the same slot mapping used in Begin() — PS slot N → DS slot M.
    ctx->PSGetShaderResources(2,  1, &slot.srvs[0]);
    ctx->PSGetShaderResources(0,  1, &slot.srvs[1]);
    ctx->PSGetShaderResources(3,  1, &slot.srvs[2]);
    ctx->PSGetShaderResources(6,  1, &slot.srvs[3]);
    ctx->PSGetShaderResources(8,  1, &slot.srvs[4]);
    ctx->PSGetShaderResources(10, 1, &slot.srvs[5]);
    ctx->PSGetShaderResources(5,  1, &slot.srvs[6]);

    // Snapshot CONTENT (not pointer) of cb0 buffers. Kenshi re-Maps these
    // DYNAMIC buffers per pass, so the buffer pointer's content changes
    // between this capture and the blood draw — must copy out now.
    slot.vsCb0Slot = gTerrainWvpCbSlot;
    slot.psCb0Data.clear();
    slot.vsCb0Data.clear();
    {
        std::lock_guard<std::mutex> lock(gCbMutex);
        ID3D11Buffer* psCb0 = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &psCb0);
        if (psCb0)
        {
            if (gTrackedCbs.insert(psCb0).second) BloomAdd(psCb0);
            auto it = gCbShadow.find(psCb0);
            if (it != gCbShadow.end() && !it->second.empty())
                slot.psCb0Data = it->second;
            psCb0->Release();
        }
        ID3D11Buffer* vsCb0 = nullptr;
        ctx->VSGetConstantBuffers(slot.vsCb0Slot, 1, &vsCb0);
        if (vsCb0)
        {
            if (gTrackedCbs.insert(vsCb0).second) BloomAdd(vsCb0);
            auto it = gCbShadow.find(vsCb0);
            if (it != gCbShadow.end() && !it->second.empty())
                slot.vsCb0Data = it->second;
            vsCb0->Release();
        }
    }

    slot.terrainPs = ps;
    slot.frame     = gFrameNumber;
}

// Drop all cached terrain VB contexts (release every AddRef'd resource).
// Called from Shutdown so we don't leak SRV/CB refs across module unload.
static void ReleaseAllTerrainVbCtx()
{
    for (auto& kv : gVbTerrainCtx)
    {
        for (int i = 0; i < 7; i++)
            if (kv.second.srvs[i]) kv.second.srvs[i]->Release();
    }
    gVbTerrainCtx.clear();
    if (gBloodPsCb0Pool) { gBloodPsCb0Pool->Release(); gBloodPsCb0Pool = nullptr; gBloodPsCb0PoolSize = 0; }
    if (gBloodVsCb0Pool) { gBloodVsCb0Pool->Release(); gBloodVsCb0Pool = nullptr; gBloodVsCb0PoolSize = 0; }
}

// Ensure a pooled DYNAMIC buffer is sized for `bytes` and Map+upload `data`
// into it. Used per-blood-draw to push the captured terrain cb0 snapshot
// onto the GPU without touching the live PS/VS cb0 buffer.
static ID3D11Buffer* UploadToPoolBuffer(ID3D11DeviceContext* ctx,
                                        ID3D11Buffer** poolPtr,
                                        UINT* poolSizePtr,
                                        const std::vector<uint8_t>& data)
{
    if (data.empty()) return nullptr;
    UINT need = (UINT)data.size();
    if (!*poolPtr || *poolSizePtr < need)
    {
        if (*poolPtr) { (*poolPtr)->Release(); *poolPtr = nullptr; }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return nullptr;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = (need + 15) & ~15u;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, poolPtr);
        dev->Release();
        if (!*poolPtr) return nullptr;
        *poolSizePtr = bd.ByteWidth;
    }
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(*poolPtr, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
    memcpy(m.pData, data.data(), need);
    ctx->Unmap(*poolPtr, 0);
    return *poolPtr;
}

static uint64_t HashBytecode(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

// Per-draw state we save and restore so the rest of the engine doesn't notice
// we ran a tess pass. Kenshi never binds HS/DS itself (it's a pre-tess D3D11
// renderer), so we treat the HS/DS pipeline state as "always null on entry"
// and skip ~10 Get*+Release calls per draw. Verified at runtime by the
// gHsDsNullVerified one-shot below; if Kenshi ever does bind HS/DS we'd see
// the warning and need to revisit.
struct SavedState
{
    D3D11_PRIMITIVE_TOPOLOGY  topo    = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*             psCb1   = nullptr;
};
SavedState gSaved;

// Controls cbuffer upload is only needed when (a) the plugin pushed new
// values via SetTerrainTessControls, or (b) the per-PS blend masks just
// changed because a different terrain PS got bound. Set true to force a
// re-upload on the next Begin().
bool                       gControlCbDirty = true;
ID3D11PixelShader*         gLastBoundPs    = nullptr;

// Diagnostic: one-shot verify HS/DS are null on entry to Begin(). If the
// game ever binds them itself, we'd need to go back to saving/restoring.
bool                       gHsDsNullVerified = false;


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

namespace detail { bool gEnabledFlag = true; }
void SetEnabled(bool enabled) { detail::gEnabledFlag = enabled; D3D11Hook::RefreshContextHooks(); }

namespace detail { bool gIsTerrainBoundFlag = false; }
namespace detail { bool gIsBloodBoundFlag   = false; }
namespace detail { std::atomic<uint64_t> gTrackedBloom[1024] = {}; }

// Cached current-shader pointers + classifications. Updated whenever the
// game calls PSSetShader / VSSetShader. We never AddRef these — they're
// pure lookup keys and only valid until the next SetShader for that stage.
static ID3D11PixelShader*  gCurrentPs              = nullptr;
static ID3D11VertexShader* gCurrentVs              = nullptr;
static bool                gCurrentVsIsTerrain     = false;
static int                 gCurrentPsBlendLevel    = -1;

// Identified terrain VS variants. Kenshi compiles terrain.hlsl main_vs twice:
//   - Terrain_Main_VP (TEXTURED define)  → full GBuffer terrain
//   - Terrain_VP      (no define)        → blood-on-terrain re-draw
// Both share input layout. We need to know which is which because the blood
// pass uses Terrain_VP, which outputs fewer interpolators than our HS expects.
// Workaround: at blood-draw time, swap VS to the TEXTURED variant + run tess.
static ID3D11VertexShader* gTerrainMainVs  = nullptr;  // TEXTURED variant
static ID3D11VertexShader* gTerrainBloodVs = nullptr;  // no-TEXTURED variant

// Blood PS: identified by having a "bloodTex" bound resource. Set in
// OnPixelShaderCreated. Used by OnPsBound to flip gIsBloodBoundFlag.
static ID3D11PixelShader*  gBloodPs        = nullptr;

static void RecomputeIsTerrainBound()
{
    detail::gIsTerrainBoundFlag =
        gCurrentVsIsTerrain && gCurrentPsBlendLevel >= 0;
    // Blood-on-terrain pass: re-draws terrain mesh with depth_func=equal
    // through the no-TEXTURED VS variant. We need to apply the same
    // displacement so depths match the displaced GBuffer terrain.
    detail::gIsBloodBoundFlag =
        gCurrentVs && gCurrentVs == gTerrainBloodVs &&
        gCurrentPs && gCurrentPs == gBloodPs;
}

void OnPsBound(ID3D11PixelShader* ps)
{
    ZoneScoped;
    gCurrentPs = ps;
    // Single-entry cache: many consecutive draws share the same PS (e.g.,
    // GBuffer terrain draws across many chunks all use the same biome PS).
    // The hashmap lookup is fast (no mutex) but still ~50ns × 2000 binds/
    // frame = 100µs that we can skip when the call is a redundant rebind.
    static ID3D11PixelShader* sLastPs = nullptr;
    static int                sLastBlendLevel = -1;
    if (ps == sLastPs)
    {
        gCurrentPsBlendLevel = sLastBlendLevel;
    }
    else if (ps)
    {
        auto it = gPsBlendLevel.find(ps);
        gCurrentPsBlendLevel = (it != gPsBlendLevel.end()) ? it->second : -1;
        sLastPs              = ps;
        sLastBlendLevel      = gCurrentPsBlendLevel;
    }
    else
    {
        gCurrentPsBlendLevel = -1;
        sLastPs              = nullptr;
        sLastBlendLevel      = -1;
    }
    RecomputeIsTerrainBound();
}

void OnVsBound(ID3D11VertexShader* vs)
{
    ZoneScoped;
    gCurrentVs = vs;
    // Direct pointer compare against the two terrain main_vs variants we
    // care about (TEXTURED and no-TEXTURED). Avoids the ShaderDatabase
    // mutex+hashmap on every VSSetShader (~2000/frame in Kenshi). Other
    // "terrain" shaders (shadow_vs etc.) aren't tessellated by us so we
    // don't need to flag them.
    gCurrentVsIsTerrain = vs &&
        (vs == gTerrainMainVs || vs == gTerrainBloodVs);
    RecomputeIsTerrainBound();
}


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

bool GetCameraPos(float outXYZ[3])
{
    if (!gHaveCameraPos || !outXYZ) return false;
    outXYZ[0] = gLastCameraPos[0];
    outXYZ[1] = gLastCameraPos[1];
    outXYZ[2] = gLastCameraPos[2];
    return true;
}

// Called by EffectLoader::HostSetTerrainTessControls after writing into the
// Controls struct. Marks the GPU cbuffer dirty so Begin() re-uploads on its
// next call. Without this, Begin() would either always Map/Unmap (wasted work
// most draws) or never re-upload (stale GPU state after a GUI change).
void MarkControlsDirty() { gControlCbDirty = true; }

void OnFrameEnd()
{
    // Bump frame counter so CaptureTerrainVbCtx tags entries with the new
    // frame, and TryDrawTessellatedBloodImpl can ignore stale entries from
    // last frame (chunks that were tessellated then but skipped now).
    gFrameNumber++;

    // Bloom only contains CB pointers now (IB/staging shadow removed).
    // CBs are a small fixed set (~5) that never goes stale, so no sweep needed.

    // Rotate the adaptive blood-capture flag: if blood drew this frame,
    // capture next frame too. If not, we'll skip capture entirely and
    // save ~10 COM calls per terrain chunk. First blood frame after a
    // long gap can't tess (no cache yet) but subsequent frames will.
    gBloodDrewLastFrame  = gBloodDrewThisFrame;
    gBloodDrewThisFrame  = false;

    // Replenish per-frame VB-readback budget. Cache misses do at most
    // this many CopyResource+Map READs per frame; uncached VBs beyond
    // the budget fall through to full tess until later frames catch up.
    gVbCaptureBudget = kVbCaptureBudgetPerFrame;

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
    // bindings. Also detect the blood PS (has a "bloodTex" bound resource).
    int blendLevel = -1;
    bool isBloodPs = false;
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
        // Blood PS (deferred/blood.hlsl `terrain` entry) has exactly one
        // texture binding named `bloodTex`. Catches it independent of source.
        for (UINT j = 0; j < sd.BoundResources; j++)
        {
            D3D11_SHADER_INPUT_BIND_DESC ib = {};
            refl->GetResourceBindingDesc(j, &ib);
            if (ib.Name && ib.Type == D3D_SIT_TEXTURE &&
                strcmp(ib.Name, "bloodTex") == 0)
            {
                isBloodPs = true;
                break;
            }
        }
        refl->Release();
    }
    gPsBlendLevel[ps] = blendLevel;
    if (isBloodPs)
    {
        gBloodPs = ps;
        Log("TerrainTess: blood PS = %p", ps);
    }

    // If a BLEND-defines record was stashed by OnTerrainPsCompiled (called
    // earlier from the D3DCompile hook), expand it into the pre-built mask
    // triplet used per draw. Skip non-terrain PSes (level < 0) — they never
    // hit the tess path so caching for them wastes memory and lookups.
    if (blendLevel >= 0)
    {
        auto bdIt = gBlendDefsByHash.find(HashBytecode(bytecode, size));
        if (bdIt != gBlendDefsByHash.end())
        {
            PsBlendMasks masks;
            const PsBlendDefines& bd = bdIt->second;
            if (bd.b1 >= 0 && bd.b1 < 4) masks.m[0][bd.b1] = 1.0f;
            if (bd.b2 >= 0 && bd.b2 < 4) masks.m[1][bd.b2] = 1.0f;
            if (bd.b3 >= 0 && bd.b3 < 4) masks.m[2][bd.b3] = 1.0f;
            gPsBlendMasks[ps] = masks;
            gBlendDefsByHash.erase(bdIt);
        }
        else
        {
            // No #define record means a level-0 terrain PS (no BLEND extension
            // layers). Insert a zero entry so the Begin() lookup is still a
            // single hash hit instead of a miss + branch.
            gPsBlendMasks[ps] = PsBlendMasks{};
        }

        // Diagnostic: reflect each unique terrain PS exactly once at creation
        // time (cheaper than per-draw reflection in Begin()). Use the
        // already-stashed bytecode so we don't re-read it.
        Log("TerrainTess: registered terrain PS=%p blendLevel=%d bytecodeSize=%zu",
            ps, blendLevel, size);
        ID3D11ShaderReflection* refl = nullptr;
        if (SUCCEEDED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, (void**)&refl)) && refl)
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
                    // Capture the offset of the PS-side cameraPos uniform —
                    // we use it for CPU-side per-chunk distance culling.
                    // The PS shader actually populates this (unlike the VS
                    // variant's cameraPos which reads as zeros).
                    if (vd.Name && strcmp(vd.Name, "cameraPos") == 0 &&
                        gTerrainPsCameraPosOffset < 0)
                    {
                        gTerrainPsCameraPosOffset = (int)vd.StartOffset;
                        Log("    => PS cameraPos found, will use for CPU skip");
                    }
                }
            }
            refl->Release();
        }
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

void OnVertexShaderCreated(const void* bytecode, size_t size, ID3D11VertexShader* vs)
{
    if (!bytecode || size == 0 || !vs) return;

    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, (void**)&refl)) || !refl)
        return;

    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);

    // Ogre's $Params cbuffer layout is per-shader — each shader's $Params
    // only contains the auto-params it actually declared. So the offset of
    // worldViewProjMatrix varies between shaders, and we MUST be reading
    // from an actual terrain VS or the offset will be wrong.
    //
    // Terrain VS is identified by `overlayData` / `biomeData` — these are
    // cbuffer VARIABLES in the terrain shaders (verified in diag logs as
    // `overlayData@160`), not bound resources. Walk every cb's variables.
    bool isTerrainVs = false;
    for (UINT i = 0; i < sd.ConstantBuffers && !isTerrainVs; i++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC bd = {};
        cb->GetDesc(&bd);
        for (UINT v = 0; v < bd.Variables; v++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vd = {};
            if (FAILED(var->GetDesc(&vd))) continue;
            if (vd.Name && (strcmp(vd.Name, "overlayData") == 0 ||
                            strcmp(vd.Name, "biomeData")   == 0))
            {
                isTerrainVs = true;
                break;
            }
        }
    }
    if (!isTerrainVs) { refl->Release(); return; }

    // Distinguish TEXTURED vs no-TEXTURED variant by output signature:
    //   TEXTURED outputs TEXCOORD2..8 (oTex0/1, uvblend, oTexV, oWvpCol*).
    //   no-TEXTURED outputs only TEXCOORD0,1 (oNormal, oWorldPos).
    bool hasTexcoord2 = false;
    for (UINT i = 0; i < sd.OutputParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        if (FAILED(refl->GetOutputParameterDesc(i, &p))) continue;
        if (p.SemanticName && strcmp(p.SemanticName, "TEXCOORD") == 0 &&
            p.SemanticIndex == 2)
        {
            hasTexcoord2 = true;
            break;
        }
    }
    if (hasTexcoord2)
    {
        gTerrainMainVs = vs;
        Log("TerrainTess: terrain main VS (TEXTURED) = %p", vs);
    }
    else
    {
        gTerrainBloodVs = vs;
        Log("TerrainTess: terrain blood VS (no-TEXTURED) = %p", vs);
    }

    // Dump every $Params variable on the first terrain VS we see so we can
    // identify which auto-param actually varies per chunk. The per-chunk
    // position signal is whichever matrix has a translation column that
    // differs between draws — typically `worldMatrix`, but in some Ogre
    // configurations (terrain that bakes positions into the VB) it might
    // not exist or be identity. Dump tells us what's available.
    for (UINT i = 0; i < sd.ConstantBuffers; i++)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC bd = {};
        cb->GetDesc(&bd);
        D3D11_SHADER_INPUT_BIND_DESC ibd = {};
        if (FAILED(refl->GetResourceBindingDescByName(bd.Name, &ibd))) continue;
        Log("TerrainTess: terrain VS cb '%s' (slot b%u, size %u, %u vars):",
            bd.Name, ibd.BindPoint, bd.Size, bd.Variables);
        for (UINT v = 0; v < bd.Variables; v++)
        {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vd = {};
            if (FAILED(var->GetDesc(&vd))) continue;
            Log("    %-32s offset=%4u size=%4u", vd.Name, vd.StartOffset, vd.Size);

            if (vd.Name && strcmp(vd.Name, "worldViewProjMatrix") == 0)
            {
                gTerrainWvpOffset = (int)vd.StartOffset;
                gTerrainWvpCbSlot = ibd.BindPoint;
            }
            if (vd.Name && strcmp(vd.Name, "worldMatrix") == 0 &&
                gTerrainWorldMatrixOffset < 0)
            {
                gTerrainWorldMatrixOffset = (int)vd.StartOffset;
            }
            // worldOffset is the per-chunk world position (vec3). The VS
            // adds it to local-space vertex positions to get world coords.
            // Track PER-VS because not every terrain VS variant has it,
            // and the offset differs between variants.
            if (vd.Name && strcmp(vd.Name, "worldOffset") == 0)
            {
                gPerVsOffsetOffset[vs] = (int)vd.StartOffset;
                gTerrainWvpCbSlot      = ibd.BindPoint;
            }
            // cameraPos lets us compute distance from camera (instead of
            // distance from world origin) for the skip threshold.
            if (vd.Name && strcmp(vd.Name, "cameraPos") == 0)
            {
                gPerVsCameraPosOffset[vs] = (int)vd.StartOffset;
                gTerrainWvpCbSlot         = ibd.BindPoint;
            }
        }
    }
    {
        auto it = gPerVsOffsetOffset.find(vs);
        int worldOff = (it != gPerVsOffsetOffset.end()) ? it->second : -1;
        Log("TerrainTess: VS %p — WVP offset = %d, worldOffset offset = %d "
            "(-1 = not present)", vs, gTerrainWvpOffset, worldOff);
    }
    refl->Release();
}

void OnContextMap(ID3D11Resource* res, void* mappedPtr)
{
    if (!res || !mappedPtr) return;
    // Bloom-filter bailout: ~99% of Map calls are for buffers we never
    // track. Single atomic load + bit test, no mutex.
    if (!BloomTest(res)) return;
    ZoneScoped;
    // Bloom false-positives are rare but real, and saves-load destroys old
    // buffers + reallocates memory — a NEW texture can land at an old
    // tracked buffer's pointer address, get a bloom hit, and we'd then
    // call buf->GetDesc() on the texture (different vtable, crash). QI
    // verifies it really is an ID3D11Buffer before we proceed.
    ID3D11Buffer* buf = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf)) || !buf) return;

    std::lock_guard<std::mutex> lock(gCbMutex);
    if (gTrackedCbs.count(buf)) { gCbPending[buf] = mappedPtr; }
    buf->Release();
}

void OnVertexBufferCreated(ID3D11Buffer* vb, const void* initialData)
{
    if (!vb || !initialData) return;
    const float* p = reinterpret_cast<const float*>(initialData);
    std::lock_guard<std::mutex> lock(gCbMutex);
    gVbPosCache[vb] = VbPos{ p[0], p[1], p[2] };
}

// OnIndexBufferCreated, OnStagingBufferCreated, OnCopyResource, and
// OnIndexBufferUpdate removed — the GPU strip→list compute shader reads
// directly from the game's IB via CopySubresourceRegion at draw time,
// eliminating the need for CPU-side IB shadow infrastructure.

void OnContextUnmap(ID3D11Resource* res)
{
    if (!res) return;
    if (!BloomTest(res)) return;
    ZoneScoped;
    // QI verifies the bloom-hit pointer really is a buffer (saves-load can
    // realloc a texture at an old tracked buffer's address). Without this,
    // the GetDesc below would invoke a wrong vtable slot and crash.
    ID3D11Buffer* buf = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf)) || !buf) return;

    std::lock_guard<std::mutex> lock(gCbMutex);

    auto cbIt = gCbPending.find(buf);
    if (cbIt != gCbPending.end())
    {
        ZoneScopedN("OnContextUnmap.cb");
        void* src = cbIt->second;
        gCbPending.erase(cbIt);

        D3D11_BUFFER_DESC bd = {};
        buf->GetDesc(&bd);
        auto& shadow = gCbShadow[buf];
        shadow.resize(bd.ByteWidth);
        memcpy(shadow.data(), src, bd.ByteWidth);
    }
    buf->Release();
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

    // Each stage source = shared common chunk (structs / textures / cbuffers /
    // helpers including ComputeDisplacementH) + the stage-specific entry.
    std::string hsSrc = std::string(kCommonShader) + kHullEntry;
    std::string dsSrc = std::string(kCommonShader) + kDomainEntry;

    ID3DBlob* hsBlob = nullptr;
    if (!CompileShader(hsSrc.c_str(), "hs_5_0", &hsBlob))
        return;

    ID3DBlob* dsBlob = nullptr;
    if (!CompileShader(dsSrc.c_str(), "ds_5_0", &dsBlob))
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

    // Blood-pass DSS: tolerate sub-ulp depth drift at grazing angles. See
    // gBloodDss declaration for rationale.
    {
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable    = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
        dsd.StencilEnable  = FALSE;
        device->CreateDepthStencilState(&dsd, &gBloodDss);
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

    // Compile the strip→list compute shader for GPU-side IB conversion.
    {
        ID3DBlob* csBlob = nullptr;
        if (CompileShader(kStripToListCS, "cs_5_0", &csBlob))
        {
            hr = device->CreateComputeShader(csBlob->GetBufferPointer(),
                                             csBlob->GetBufferSize(),
                                             nullptr, &gStripCS);
            csBlob->Release();
            if (FAILED(hr))
            {
                Log("TerrainTess: CreateComputeShader failed (0x%08X)", hr);
                gStripCS = nullptr;
            }
        }
    }

    Log("TerrainTess: HS+DS compiled and ready (CS %s)",
        gStripCS ? "ok" : "FAILED");
}

void Shutdown()
{
    if (gHs)          { gHs->Release();          gHs = nullptr; }
    if (gDs)          { gDs->Release();          gDs = nullptr; }
    if (gLinearWrap)  { gLinearWrap->Release();  gLinearWrap = nullptr; }
    if (gControlCb)   { gControlCb->Release();   gControlCb = nullptr; }
    if (gBloodDss)    { gBloodDss->Release();    gBloodDss = nullptr; }
    if (gWireframeRs)  { gWireframeRs->Release();  gWireframeRs = nullptr; }
    if (gStripCS)      { gStripCS->Release();      gStripCS = nullptr; }
    if (gStripCB)      { gStripCB->Release();      gStripCB = nullptr; }
    if (gStripSrcSRV)  { gStripSrcSRV->Release();  gStripSrcSRV = nullptr; }
    if (gStripSrc)     { gStripSrc->Release();     gStripSrc = nullptr; }
    if (gStripDstUAV)  { gStripDstUAV->Release();  gStripDstUAV = nullptr; }
    if (gStripDst)     { gStripDst->Release();     gStripDst = nullptr; }
    if (gStripArgsUAV) { gStripArgsUAV->Release(); gStripArgsUAV = nullptr; }
    if (gStripArgs)    { gStripArgs->Release();    gStripArgs = nullptr; }
    gStripSrcCap = 0; gStripSrcFmt = DXGI_FORMAT_UNKNOWN;
    gStripDstCap = 0; gStripDstFmt = DXGI_FORMAT_UNKNOWN;
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
    gPsBlendMasks.clear();
    gBlendDefsByHash.clear();
    {
        std::lock_guard<std::mutex> lock(gCbMutex);
        gTrackedCbs.clear();
        gCbShadow.clear();
        gCbPending.clear();
    }
    gTerrainWvpOffset = -1;
    gTerrainWorldMatrixOffset = -1;
    gTerrainPsCameraPosOffset = -1;
    gPerVsOffsetOffset.clear();
    gPerVsCameraPosOffset.clear();
    gVbPosCache.clear();
    if (gVbPosStaging) { gVbPosStaging->Release(); gVbPosStaging = nullptr; }
    for (int i = 0; i < kCbStagingSlots; ++i)
    {
        if (gCamStaging[i])   { gCamStaging[i]->Release();   gCamStaging[i]   = nullptr; }
        if (gShiftStaging[i]) { gShiftStaging[i]->Release(); gShiftStaging[i] = nullptr; }
    }
    gCbStagingWriteSlot = 0;
    gCbStagingPrimed    = false;
    ReleaseAllTerrainVbCtx();
    gTerrainMainVs  = nullptr;
    gTerrainBloodVs = nullptr;
    gBloodPs        = nullptr;
}


// ============================================================================
// Internal: filter draws + IB conversion
// ============================================================================

namespace {

bool IsTerrainShaderBound(ID3D11DeviceContext* ctx)
{
    // gEnabled is also checked at the hook call site so we never get here
    // when disabled — this is a defensive backstop only.
    if (!detail::gEnabledFlag) return false;

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

// Ensure a pooled D3D11 buffer is at least `needBytes`. Recreates on growth;
// geometric growth amortizes the recreate cost. `tmpl` is the create-time
// descriptor template, ByteWidth gets overwritten.
static bool EnsurePooledBuffer(ID3D11Device* device,
                               ID3D11Buffer** buf, UINT* cap,
                               UINT needBytes, const D3D11_BUFFER_DESC& tmpl)
{
    if (*buf && *cap >= needBytes) return true;
    if (*buf) { (*buf)->Release(); *buf = nullptr; }

    UINT newCap = *cap ? *cap : 4096;
    while (newCap < needBytes) newCap *= 2;

    D3D11_BUFFER_DESC bd = tmpl;
    bd.ByteWidth = newCap;
    if (FAILED(device->CreateBuffer(&bd, nullptr, buf))) { *cap = 0; return false; }
    *cap = newCap;
    return true;
}

// GPU strip→list: copies the strip sub-range from the game IB to a
// DEFAULT+SRV buffer, dispatches the compute shader, and outputs a
// list IB + indirect draw args. Zero CPU memcpy, zero GPU readback.
bool GpuStripToList(ID3D11DeviceContext* ctx,
                    ID3D11Buffer* origIB, DXGI_FORMAT origFormat,
                    UINT origByteOffset,
                    UINT stripIndexCount, UINT stripStartIndex,
                    INT baseVertex,
                    ID3D11Buffer** outListIB, ID3D11Buffer** outIndirectArgs)
{
    ZoneScopedN("Tess.GpuStripConvert");

    if (!origIB || stripIndexCount < 3 || !gStripCS) return false;
    if (origFormat != DXGI_FORMAT_R16_UINT && origFormat != DXGI_FORMAT_R32_UINT)
        return false;

    UINT indexSize    = (origFormat == DXGI_FORMAT_R16_UINT) ? 2 : 4;
    UINT stripBytes   = stripIndexCount * indexSize;
    UINT srcStartByte = origByteOffset + stripStartIndex * indexSize;
    UINT triCount     = stripIndexCount - 2;
    UINT dstBytes     = triCount * 3 * indexSize;

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return false;

    // --- source buffer (DEFAULT + SRV) ---
    bool srcRealloced = (stripBytes > gStripSrcCap);
    D3D11_BUFFER_DESC srcTmpl = {};
    srcTmpl.Usage     = D3D11_USAGE_DEFAULT;
    srcTmpl.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (srcRealloced && gStripSrcSRV) { gStripSrcSRV->Release(); gStripSrcSRV = nullptr; }
    if (!EnsurePooledBuffer(device, &gStripSrc, &gStripSrcCap, stripBytes, srcTmpl))
    { device->Release(); return false; }

    if (!gStripSrcSRV || gStripSrcFmt != origFormat)
    {
        if (gStripSrcSRV) { gStripSrcSRV->Release(); gStripSrcSRV = nullptr; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = origFormat;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements  = gStripSrcCap / indexSize;
        if (FAILED(device->CreateShaderResourceView(gStripSrc, &sd, &gStripSrcSRV)))
        { device->Release(); return false; }
        gStripSrcFmt = origFormat;
    }

    D3D11_BOX srcBox = {};
    srcBox.left = srcStartByte; srcBox.right = srcStartByte + stripBytes;
    srcBox.bottom = 1; srcBox.back = 1;
    ctx->CopySubresourceRegion(gStripSrc, 0, 0, 0, 0, origIB, 0, &srcBox);

    // --- destination buffer (DEFAULT + IB + UAV) ---
    if (dstBytes > gStripDstCap)
    {
        if (gStripDstUAV) { gStripDstUAV->Release(); gStripDstUAV = nullptr; }
        if (gStripDst)    { gStripDst->Release();    gStripDst = nullptr; }
        gStripDstFmt = DXGI_FORMAT_UNKNOWN;

        UINT newCap = gStripDstCap ? gStripDstCap : 4096;
        while (newCap < dstBytes) newCap *= 2;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = newCap;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &gStripDst)))
        { gStripDstCap = 0; device->Release(); return false; }
        gStripDstCap = newCap;
    }

    if (!gStripDstUAV || gStripDstFmt != origFormat)
    {
        if (gStripDstUAV) { gStripDstUAV->Release(); gStripDstUAV = nullptr; }
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format             = origFormat;
        ud.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = gStripDstCap / indexSize;
        if (FAILED(device->CreateUnorderedAccessView(gStripDst, &ud, &gStripDstUAV)))
        { device->Release(); return false; }
        gStripDstFmt = origFormat;
    }

    // --- indirect args buffer (DEFAULT + UAV + DRAWINDIRECT_ARGS) ---
    if (!gStripArgs)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 20;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS |
                       D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &gStripArgs)))
        { device->Release(); return false; }

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format             = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 5;
        ud.Buffer.Flags       = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device->CreateUnorderedAccessView(gStripArgs, &ud, &gStripArgsUAV)))
        { gStripArgs->Release(); gStripArgs = nullptr; device->Release(); return false; }
    }

    // --- constant buffer ---
    if (!gStripCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &gStripCB)))
        { device->Release(); return false; }
    }

    // Upload params + reset indirect args counter
    UINT cbData[4] = { stripIndexCount, 0, 0, 0 };
    ctx->UpdateSubresource(gStripCB, 0, nullptr, cbData, 0, 0);

    struct { UINT ic; UINT inst; UINT si; INT bv; UINT sInst; }
        argsInit = { 0, 1, 0, baseVertex, 0 };
    ctx->UpdateSubresource(gStripArgs, 0, nullptr, &argsInit, 0, 0);

    // --- dispatch ---
    ctx->CSSetShader(gStripCS, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &gStripCB);
    ctx->CSSetShaderResources(0, 1, &gStripSrcSRV);
    ID3D11UnorderedAccessView* uavs[2] = { gStripDstUAV, gStripArgsUAV };
    UINT initCounts[2] = { (UINT)-1, (UINT)-1 };
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, initCounts);

    ctx->Dispatch((triCount + 255) / 256, 1, 1);

    // Unbind so the resources are available as IB / indirect args
    ID3D11ShaderResourceView*  nullSRV  = nullptr;
    ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
    ctx->CSSetShaderResources(0, 1, &nullSRV);
    ctx->CSSetUnorderedAccessViews(0, 2, nullUAVs, initCounts);

    device->Release();
    *outListIB       = gStripDst;
    *outIndirectArgs = gStripArgs;
    return true;
}

} // anonymous namespace


// ============================================================================
// Begin / End — set up and tear down DS pipeline state per draw
// ============================================================================

void Begin(ID3D11DeviceContext* ctx)
{
    ZoneScoped;
    // Save just enough state to put things back when we're done. Kenshi
    // never uses HS/DS/GS or their cbuffers/SRVs/samplers itself, so we
    // don't bother snapshotting any of that — End() just nulls them out.
    // PS cb1 IS used by the patched terrain PS for debug-view mode, so it
    // gets saved/restored. IA topology is restored because the game expects
    // TRIANGLELIST/TRIANGLESTRIP after this draw.
    ctx->IAGetPrimitiveTopology(&gSaved.topo);
    ctx->PSGetConstantBuffers(1, 1, &gSaved.psCb1);

    // One-shot sanity check: verify HS/DS really are null on entry. If
    // anything else ever binds them, we'd be silently corrupting that
    // pipeline by skipping save/restore — better to know early.
    if (!gHsDsNullVerified)
    {
        ID3D11HullShader*   probeHs = nullptr;
        ID3D11DomainShader* probeDs = nullptr;
        ctx->HSGetShader(&probeHs, nullptr, 0);
        ctx->DSGetShader(&probeDs, nullptr, 0);
        if (probeHs || probeDs)
        {
            Log("TerrainTess: WARNING — HS/DS were NON-NULL on Begin entry "
                "(hs=%p ds=%p). The fast-path assumes Kenshi never binds them. "
                "Output may be corrupt; please report.",
                probeHs, probeDs);
        }
        if (probeHs) probeHs->Release();
        if (probeDs) probeDs->Release();
        gHsDsNullVerified = true;
    }

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ctx->HSSetShader(gHs, nullptr, 0);
    ctx->DSSetShader(gDs, nullptr, 0);
    // Defensive: clear any GS that might be bound between DS and rasterizer.
    ID3D11GeometryShader* nullGs = nullptr;
    ctx->GSSetShader(nullGs, nullptr, 0);

    // Snapshot the current PS resources keyed by VB so the blood-on-terrain
    // pass (same VB, but PS now sampling bloodTex instead of biome textures)
    // can re-bind these to HS/DS and produce matching displacement. See
    // TryDrawTessellatedBlood for the consumer side.
    //
    // Adaptive: only do this work when blood actually drew last frame
    // (active combat). Skips the ~10 COM calls + mutex + memcpys per
    // chunk during exploration where no blood is on terrain.
    if (gBloodDrewLastFrame)
    {
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, offset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        if (vb)
        {
            CaptureTerrainVbCtx(ctx, vb, gCurrentPs);
            vb->Release();
        }
    }

    // Mirror PS cb0 (terrain $Globals: scalesA/B/C, slopeMin/Max/Blend, etc.)
    // to both HS and DS cb0 — both stages call the shared ComputeDisplacementH
    // helper which reads PS uniforms via the PsTerrainCb mirror.
    //
    // For blend-mask lookup we use the cached gCurrentPs pointer (updated by
    // PSSetShader hook) instead of doing PSGetShader+Release per draw. Saves
    // a COM call on every terrain draw — small but hundreds-of-draws-per-frame.
    ID3D11PixelShader* curPs = gCurrentPs;
    {
        ID3D11Buffer* psCb0 = nullptr;
        ctx->PSGetConstantBuffers(0, 1, &psCb0);
        ctx->HSSetConstantBuffers(0, 1, &psCb0);
        ctx->DSSetConstantBuffers(0, 1, &psCb0);
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
        ctx->HSSetShaderResources(0, 7, psSrvs);
        ctx->DSSetShaderResources(0, 7, psSrvs);
        for (int i = 0; i < 7; i++) if (psSrvs[i]) psSrvs[i]->Release();
    }
    ctx->HSSetSamplers(0, 1, &gLinearWrap);
    ctx->DSSetSamplers(0, 1, &gLinearWrap);

    // Load the per-PS BLEND# channel masks the DS uses to weight BLEND1/2/3
    // layers exactly as the PS does. Masks are precomputed at PS creation —
    // here we just memcpy the 48-byte triplet (or zero it if the PS isn't
    // registered). Only when the bound PS actually changed since the last
    // tess draw do we mark the cbuffer dirty — consecutive draws of the
    // same PS use the same masks and the GPU copy is still valid.
    if (curPs != gLastBoundPs)
    {
        if (curPs)
        {
            auto it = gPsBlendMasks.find(curPs);
            if (it != gPsBlendMasks.end())
                memcpy(gControls.blend1Mask, it->second.m, sizeof(it->second.m));
            else
                memset(gControls.blend1Mask, 0, sizeof(gControls.blend1Mask) * 3);
        }
        else
        {
            memset(gControls.blend1Mask, 0, sizeof(gControls.blend1Mask) * 3);
        }
        gLastBoundPs    = curPs;
        gControlCbDirty = true;
    }
    // curPs comes from the cached gCurrentPs pointer — we don't own a ref,
    // so no Release.

    // Upload TessControl cbuffer only when something changed. With ~hundreds
    // of terrain draws per frame and the controls staying constant nearly
    // always, this saves the Map+memcpy+Unmap on >99% of draws.
    if (gControlCb)
    {
        if (gControlCbDirty)
        {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(gControlCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, &gControls, sizeof(gControls));
                ctx->Unmap(gControlCb, 0);
                gControlCbDirty = false;
            }
        }
        ctx->HSSetConstantBuffers(1, 1, &gControlCb);
        ctx->DSSetConstantBuffers(1, 1, &gControlCb);
        // PS reads gDebugViewMode for the debug-view patch.
        ctx->PSSetConstantBuffers(1, 1, &gControlCb);
    }
}

void End(ID3D11DeviceContext* ctx)
{
    ZoneScoped;
    // Restore the IA topology and PS cb1 the game expects. NULL the HS/DS
    // pipeline (we know it was null on entry — see one-shot check in Begin)
    // so nothing downstream accidentally runs a tess pass.
    ctx->IASetPrimitiveTopology(gSaved.topo);
    ctx->PSSetConstantBuffers(1, 1, &gSaved.psCb1);
    if (gSaved.psCb1) { gSaved.psCb1->Release(); gSaved.psCb1 = nullptr; }
    gSaved.topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    ID3D11HullShader*   nullHs = nullptr;
    ID3D11DomainShader* nullDs = nullptr;
    ctx->HSSetShader(nullHs, nullptr, 0);
    ctx->DSSetShader(nullDs, nullptr, 0);
}

// Blood-pass variants of Begin/End. Differences vs the terrain Begin/End:
//   - Uses cached SRVs + cb0 from `cached` (the blood PS has its own
//     resources bound to PS slots that aren't terrain — we'd corrupt the
//     DS displacement if we mirrored from PS).
//   - Does NOT touch PS cb1 (blood owns it for the projection-info uniform).
//   - Blend masks come from the cached terrain PS (which has the right
//     BLEND# defines), NOT from the currently-bound blood PS.
//   - Overrides depth-stencil state to LESS_EQUAL/no-write so tess-introduced
//     sub-ulp depth drift at grazing angles doesn't make the blood mesh
//     fail the original equal test (the dominant cause of "blood disappears
//     at grazing angles" in vanilla; tess accentuates it).
// Caller is responsible for saving/restoring topology + depth state since
// BeginBlood doesn't snapshot to gSaved.
void BeginBlood(ID3D11DeviceContext* ctx, const TerrainVbCtx* cached)
{
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ctx->HSSetShader(gHs, nullptr, 0);
    ctx->DSSetShader(gDs, nullptr, 0);
    ID3D11GeometryShader* nullGs = nullptr;
    ctx->GSSetShader(nullGs, nullptr, 0);

    if (gBloodDss) ctx->OMSetDepthStencilState(gBloodDss, 0);

    // Upload the captured terrain-time PS cb0 content into our pooled buffer
    // (the live cb0 has been overwritten with the blood pass's content by now).
    ID3D11Buffer* psCb0 = UploadToPoolBuffer(ctx, &gBloodPsCb0Pool,
                                              &gBloodPsCb0PoolSize,
                                              cached->psCb0Data);
    if (psCb0)
    {
        ctx->HSSetConstantBuffers(0, 1, &psCb0);
        ctx->DSSetConstantBuffers(0, 1, &psCb0);
    }
    ctx->HSSetShaderResources(0, 7,
        const_cast<ID3D11ShaderResourceView**>(cached->srvs));
    ctx->DSSetShaderResources(0, 7,
        const_cast<ID3D11ShaderResourceView**>(cached->srvs));
    ctx->HSSetSamplers(0, 1, &gLinearWrap);
    ctx->DSSetSamplers(0, 1, &gLinearWrap);

    ID3D11PixelShader* terrainPs = cached->terrainPs;
    if (terrainPs != gLastBoundPs)
    {
        if (terrainPs)
        {
            auto it = gPsBlendMasks.find(terrainPs);
            if (it != gPsBlendMasks.end())
                memcpy(gControls.blend1Mask, it->second.m, sizeof(it->second.m));
            else
                memset(gControls.blend1Mask, 0, sizeof(gControls.blend1Mask) * 3);
        }
        else
        {
            memset(gControls.blend1Mask, 0, sizeof(gControls.blend1Mask) * 3);
        }
        gLastBoundPs    = terrainPs;
        gControlCbDirty = true;
    }
    if (gControlCb)
    {
        if (gControlCbDirty)
        {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(gControlCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, &gControls, sizeof(gControls));
                ctx->Unmap(gControlCb, 0);
                gControlCbDirty = false;
            }
        }
        ctx->HSSetConstantBuffers(1, 1, &gControlCb);
        ctx->DSSetConstantBuffers(1, 1, &gControlCb);
        // Intentionally NOT touching PS cb1 — blood's projection cb lives there.
    }
}

void EndBlood(ID3D11DeviceContext* ctx)
{
    // Caller restores topology + VS + IB. We just null HS/DS so subsequent
    // non-tess draws don't accidentally run through the patch pipeline.
    ID3D11HullShader*   nullHs = nullptr;
    ID3D11DomainShader* nullDs = nullptr;
    ctx->HSSetShader(nullHs, nullptr, 0);
    ctx->DSSetShader(nullDs, nullptr, 0);
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

// Look up the cached first-vertex position for a VB. Fast path is a
// hashmap hit (populated either at CreateBuffer-with-initial-data time
// or by a prior cache-miss fallback). Cache miss → one CopyResource +
// Map(READ) to populate the entry (~1-2ms CPU/GPU sync), then cached.
// Budget-throttled to N per frame so rotations don't all-at-once stall.
bool TryGetVbFirstVertex(ID3D11DeviceContext* ctx, ID3D11Buffer* vb,
                         UINT vbBindOffset, float outPos[3])
{
    {
        std::lock_guard<std::mutex> lock(gCbMutex);
        auto it = gVbPosCache.find(vb);
        if (it != gVbPosCache.end())
        {
            outPos[0] = it->second.x;
            outPos[1] = it->second.y;
            outPos[2] = it->second.z;
            return true;
        }
    }
    if (gVbCaptureBudget <= 0) return false;

    if (!gVbPosStaging)
    {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = 16;
        bd.Usage          = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateBuffer(&bd, nullptr, &gVbPosStaging);
        dev->Release();
        if (!gVbPosStaging) return false;
    }

    D3D11_BOX box = {};
    box.left   = vbBindOffset;
    box.right  = vbBindOffset + 12;
    box.top    = 0; box.bottom = 1;
    box.front  = 0; box.back   = 1;
    ctx->CopySubresourceRegion(gVbPosStaging, 0, 0, 0, 0, vb, 0, &box);

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(gVbPosStaging, 0, D3D11_MAP_READ, 0, &m))) return false;
    const float* src = reinterpret_cast<const float*>(m.pData);
    outPos[0] = src[0];
    outPos[1] = src[1];
    outPos[2] = src[2];
    ctx->Unmap(gVbPosStaging, 0);

    {
        std::lock_guard<std::mutex> lock(gCbMutex);
        gVbPosCache[vb] = VbPos{ outPos[0], outPos[1], outPos[2] };
    }
    gVbCaptureBudget--;
    return true;
}

// Blood-on-terrain pass tessellation. The blood material redraws the terrain
// mesh with `depth_func equal` — pixels only pass when their VS-computed
// depth matches the depth buffer (which the GBuffer terrain pass filled with
// DISPLACED depths). The blood VS is the no-TEXTURED Terrain_VP variant
// which produces un-displaced positions → depths never match → invisible.
//
// Fix: temporarily swap the bound VS to the TEXTURED variant + run the
// tess pipeline using the cached terrain-pass resources for this VB. The
// blood PS reads only TEXCOORD0 (normal) and TEXCOORD1 (worldPos); our
// patched DS outputs at least those (plus extras the PS ignores), so the
// linkage works.
bool TryDrawTessellatedBloodImpl(ID3D11DeviceContext* ctx,
                                  UINT indexCount, UINT startIndex,
                                  INT baseVertex, DrawIndexedFn drawFn)
{
    ZoneScoped;
    if (!ctx || !gHs || !gDs || !drawFn) return false;
    if (!gTerrainMainVs) return false;   // never saw the TEXTURED variant

    // Bootstrap: any blood-draw attempt — successful or not — arms the next
    // frame's CaptureTerrainVbCtx via gBloodDrewLastFrame. Without this, a
    // cold-start blood sequence is locked out forever: the success path that
    // sets the flag also requires a captured ctx, but capture only fires
    // when the flag was set last frame.
    gBloodDrewThisFrame = true;

    ID3D11Buffer* vb = nullptr;
    UINT vbStride = 0, vbOffset = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
    if (!vb) return false;

    TerrainVbCtx* cached = nullptr;
    {
        auto it = gVbTerrainCtx.find(vb);
        if (it != gVbTerrainCtx.end() && !it->second.psCb0Data.empty() &&
            it->second.frame == gFrameNumber)
            cached = &it->second;
    }
    if (!cached)
    {
        // No fresh cached ctx → this VB wasn't tessellated this frame
        // (chunk past skipDistance), so its GBuffer depth is un-displaced.
        // The blood VS already produces matching un-displaced depth —
        // depth_func equal passes naturally. Fall through.
        vb->Release();
        return false;
    }

    D3D11_PRIMITIVE_TOPOLOGY savedTopo;
    ctx->IAGetPrimitiveTopology(&savedTopo);

    ID3D11Buffer* savedIb     = nullptr;
    DXGI_FORMAT   savedIbFmt  = DXGI_FORMAT_UNKNOWN;
    UINT          savedIbOff  = 0;
    ctx->IAGetIndexBuffer(&savedIb, &savedIbFmt, &savedIbOff);

    ID3D11VertexShader* savedVs = nullptr;
    ctx->VSGetShader(&savedVs, nullptr, nullptr);

    bool didStripConvert = false;
    ID3D11Buffer* stripIndirectArgs = nullptr;

    if (savedTopo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    {
        if (!savedIb)
        {
            if (savedVs) savedVs->Release();
            vb->Release();
            return false;
        }
        ID3D11Buffer* listIB = nullptr;
        if (!GpuStripToList(ctx, savedIb, savedIbFmt, savedIbOff,
                            indexCount, startIndex, baseVertex,
                            &listIB, &stripIndirectArgs))
        {
            if (savedIb) savedIb->Release();
            if (savedVs) savedVs->Release();
            vb->Release();
            return false;
        }
        ctx->IASetIndexBuffer(listIB, savedIbFmt, 0);
        didStripConvert = true;
    }
    else if (savedTopo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
    {
        if (savedIb) savedIb->Release();
        if (savedVs) savedVs->Release();
        vb->Release();
        return false;
    }

    ctx->VSSetShader(gTerrainMainVs, nullptr, 0);

    ID3D11Buffer* savedVsCb0 = nullptr;
    ID3D11Buffer* vsCb0Pool = UploadToPoolBuffer(ctx, &gBloodVsCb0Pool,
                                                  &gBloodVsCb0PoolSize,
                                                  cached->vsCb0Data);
    if (vsCb0Pool)
    {
        ctx->VSGetConstantBuffers(cached->vsCb0Slot, 1, &savedVsCb0);
        ctx->VSSetConstantBuffers(cached->vsCb0Slot, 1, &vsCb0Pool);
    }

    ID3D11DepthStencilState* savedDss = nullptr;
    UINT                     savedStencilRef = 0;
    ctx->OMGetDepthStencilState(&savedDss, &savedStencilRef);

    BeginBlood(ctx, cached);
    if (stripIndirectArgs)
        ctx->DrawIndexedInstancedIndirect(stripIndirectArgs, 0);
    else
        drawFn(ctx, indexCount, startIndex, baseVertex);
    EndBlood(ctx);
    gBloodDrewThisFrame = true;

    // Restore depth state, VS cb0, IB, topology, VS.
    ctx->OMSetDepthStencilState(savedDss, savedStencilRef);
    if (savedDss) savedDss->Release();
    if (vsCb0Pool)
    {
        ctx->VSSetConstantBuffers(cached->vsCb0Slot, 1, &savedVsCb0);
        if (savedVsCb0) savedVsCb0->Release();
    }
    if (didStripConvert)
        ctx->IASetIndexBuffer(savedIb, savedIbFmt, savedIbOff);
    ctx->IASetPrimitiveTopology(savedTopo);
    ctx->VSSetShader(savedVs, nullptr, 0);

    if (savedIb) savedIb->Release();
    if (savedVs) savedVs->Release();
    vb->Release();
    return true;
}

} // anon

bool TryDrawTessellated(ID3D11DeviceContext* ctx,
                        UINT indexCount, UINT startIndex,
                        INT baseVertex, DrawIndexedFn drawFn)
{
    ZoneScoped;
    // Blood-on-terrain pass — re-applies tess so depth matches the displaced
    // GBuffer terrain (depth_func=equal in the blood material). Handled
    // before the terrain gate because blood draws don't set gIsTerrainBoundFlag.
    if (detail::gIsBloodBoundFlag)
        return TryDrawTessellatedBloodImpl(ctx, indexCount, startIndex,
                                            baseVertex, drawFn);

    // Fast bailout for non-terrain draws — the cached flag is updated by
    // the PSSetShader/VSSetShader hooks, so this is a single bool load.
    // Without this, every non-terrain DrawIndexed in the game paid for
    // COM GetShader + Release pairs + multiple map lookups.
    if (!detail::gIsTerrainBoundFlag) return false;
    if (!ctx || !gHs || !gDs || !drawFn) return false;

    // DIAG (wireframe == 4): force-skip every terrain draw through the
    // original DrawIndexed (no HS/DS). If this recovers fps to "tess-off"
    // levels, the CPU-side skip path is sound and only the distance/matrix
    // logic needs more work. If it doesn't, the cost isn't in tess routing
    // at all and we've been chasing the wrong thing.
    if (gControls.wireframe > 3.5f && gControls.wireframe < 4.5f)
    {
        drawFn(ctx, indexCount, startIndex, baseVertex);
        return true;
    }

    // CPU-side per-chunk skip. Binding HS+DS at all costs ~5ms in busy
    // scenes regardless of how trivial they are, so once we know a chunk
    // is past where displacement matters we bypass the entire tess pipeline
    // and just submit the original draw.
    //
    // CPU-side per-chunk skip. Two pieces:
    //   1. VB[0] read → first vertex of the chunk, in world coords
    //      (cached per VB pointer; one Map READ per unique VB, throttled).
    //   2. WVP matrix from the shadowed cb → multiply against VB[0] to
    //      get the SAME clip-space w the shader's DS sees for that vertex
    //      (the value that drives the distance fade in the shader).
    //
    // abs(vposW) is then comparable across chunks: small ≈ near camera,
    // large ≈ far. Skip when > skipDistance.
    if (gTerrainWvpOffset >= 0)
    {
        ZoneScopedN("Tess.DistanceSkip");
        ID3D11Buffer* vb = nullptr;
        UINT vbStride = 0, vbBindOffset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbBindOffset);
        if (vb)
        {
            float chunkPos[3] = {};
            if (TryGetVbFirstVertex(ctx, vb, vbBindOffset, chunkPos))
            {
                // Per-frame cb extract: copy current PS/VS cb0 slices into
                // ring-buffered staging buffers, Map(READ) the previous
                // slot. Decouples the tess metric from gCbShadow → PS/VS
                // cb0 no longer need to be in the Map/Unmap tracked set
                // (unless blood is active, which still tracks them via
                // CaptureTerrainVbCtx for byte-exact replay).
                if (gFrameSkipDataFrame != gFrameNumber)
                {
                    ZoneScopedN("Tess.CbStaging");
                    gFrameSkipDataFrame = gFrameNumber;
                    gFrameHaveCam = gFrameHaveShift = false;

                    ID3D11Buffer* psCb0 = nullptr;
                    ID3D11Buffer* vsCb  = nullptr;
                    if (gTerrainPsCameraPosOffset >= 0)
                        ctx->PSGetConstantBuffers(0, 1, &psCb0);
                    if (gTerrainWorldMatrixOffset >= 0)
                        ctx->VSGetConstantBuffers(gTerrainWvpCbSlot, 1, &vsCb);

                    // Lazy-create the 16-byte staging buffers (D3D11 minimum).
                    if (!gCamStaging[0] || !gShiftStaging[0])
                    {
                        ID3D11Device* dev = nullptr;
                        ctx->GetDevice(&dev);
                        if (dev)
                        {
                            D3D11_BUFFER_DESC bd = {};
                            bd.ByteWidth      = 16;
                            bd.Usage          = D3D11_USAGE_STAGING;
                            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            for (int i = 0; i < kCbStagingSlots; ++i)
                            {
                                if (!gCamStaging[i])
                                    dev->CreateBuffer(&bd, nullptr, &gCamStaging[i]);
                                if (!gShiftStaging[i])
                                    dev->CreateBuffer(&bd, nullptr, &gShiftStaging[i]);
                            }
                            dev->Release();
                        }
                    }

                    const int writeSlot = gCbStagingWriteSlot;
                    const int readSlot  = 1 - writeSlot;

                    // Copy current frame's slices into the write slot.
                    if (psCb0 && gCamStaging[writeSlot])
                    {
                        D3D11_BOX box = {};
                        box.left   = (UINT)gTerrainPsCameraPosOffset;
                        box.right  = (UINT)gTerrainPsCameraPosOffset + 12;
                        box.top    = 0; box.bottom = 1;
                        box.front  = 0; box.back   = 1;
                        ctx->CopySubresourceRegion(gCamStaging[writeSlot], 0,
                                                    0, 0, 0, psCb0, 0, &box);
                    }
                    if (vsCb && gShiftStaging[writeSlot])
                    {
                        // worldMatrix is a 4x4 column-major. Translation is
                        // column 3 → floats at indices 12,13,14 → byte offset
                        // 48 within the matrix.
                        D3D11_BOX box = {};
                        box.left   = (UINT)gTerrainWorldMatrixOffset + 48;
                        box.right  = (UINT)gTerrainWorldMatrixOffset + 60;
                        box.top    = 0; box.bottom = 1;
                        box.front  = 0; box.back   = 1;
                        ctx->CopySubresourceRegion(gShiftStaging[writeSlot], 0,
                                                    0, 0, 0, vsCb, 0, &box);
                    }

                    if (psCb0) psCb0->Release();
                    if (vsCb)  vsCb->Release();

                    // Read the previous slot — GPU drained it a frame ago,
                    // no stall. First frame after init reads nothing.
                    if (gCbStagingPrimed)
                    {
                        D3D11_MAPPED_SUBRESOURCE mr = {};
                        if (gCamStaging[readSlot] &&
                            SUCCEEDED(ctx->Map(gCamStaging[readSlot], 0,
                                               D3D11_MAP_READ, 0, &mr)))
                        {
                            const float* p = reinterpret_cast<const float*>(mr.pData);
                            gFrameCamX = p[0]; gFrameCamY = p[1]; gFrameCamZ = p[2];
                            gFrameHaveCam = true;
                            ctx->Unmap(gCamStaging[readSlot], 0);
                        }
                        if (gShiftStaging[readSlot] &&
                            SUCCEEDED(ctx->Map(gShiftStaging[readSlot], 0,
                                               D3D11_MAP_READ, 0, &mr)))
                        {
                            const float* p = reinterpret_cast<const float*>(mr.pData);
                            gFrameShiftX = p[0]; gFrameShiftY = p[1]; gFrameShiftZ = p[2];
                            gFrameHaveShift = true;
                            ctx->Unmap(gShiftStaging[readSlot], 0);
                        }
                    }

                    gCbStagingWriteSlot = readSlot;
                    gCbStagingPrimed = true;
                }

                if (gFrameHaveCam && gFrameHaveShift)
                {
                    // Shift chunkPos into camera-relative-rebased coords
                    // (the same space cameraPos lives in). Distance metric
                    // is invariant under translation, so this is equivalent
                    // to true world-space distance.
                    float chunkRebX = chunkPos[0] + gFrameShiftX;
                    float chunkRebY = chunkPos[1] + gFrameShiftY;
                    float chunkRebZ = chunkPos[2] + gFrameShiftZ;

                    float dx = chunkRebX - gFrameCamX;
                    float dy = chunkRebY - gFrameCamY;
                    float dz = chunkRebZ - gFrameCamZ;
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);

                    // Stash actual camera world pos for the host API.
                    // True camera world = -worldMatrix.translation.
                    gLastCameraPos[0] = -gFrameShiftX;
                    gLastCameraPos[1] = -gFrameShiftY;
                    gLastCameraPos[2] = -gFrameShiftZ;
                    gHaveCameraPos = true;

                    static int sDrawCount = 0;
                    if ((sDrawCount++ % 200) == 0)
                    {
                        Log("TerrainTess: skip-sample %d  chunk=(%.0f,%.0f,%.0f) "
                            "shift=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f) "
                            "camDist=%.1f  (skipDistance=%.1f)",
                            sDrawCount,
                            chunkPos[0], chunkPos[1], chunkPos[2],
                            gFrameShiftX, gFrameShiftY, gFrameShiftZ,
                            gFrameCamX, gFrameCamY, gFrameCamZ,
                            dist, gControls.skipDistance);
                    }

                    if (gControls.debugViewMode < 2.5f &&
                        gControls.skipDistance > 0.0f &&
                        dist > gControls.skipDistance)
                    {
                        vb->Release();
                        drawFn(ctx, indexCount, startIndex, baseVertex);
                        return true;
                    }
                }
            }
            vb->Release();
        }
    }

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
        ID3D11Buffer* indirectArgs = nullptr;
        if (!GpuStripToList(ctx, origIB, origFormat, origOffset,
                            indexCount, startIndex, baseVertex,
                            &listIB, &indirectArgs))
        { origIB->Release(); return false; }
        ID3D11RasterizerState* prevRs = nullptr;
        ctx->RSGetState(&prevRs);
        ID3D11HullShader*   prevHs = nullptr; ctx->HSGetShader(&prevHs, nullptr, nullptr);
        ID3D11DomainShader* prevDs = nullptr; ctx->DSGetShader(&prevDs, nullptr, nullptr);
        if (gWireframeRs) ctx->RSSetState(gWireframeRs);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetIndexBuffer(listIB, origFormat, 0);
        ctx->DrawIndexedInstancedIndirect(indirectArgs, 0);
        ctx->IASetIndexBuffer(origIB, origFormat, origOffset);
        ctx->IASetPrimitiveTopology(topo);
        ctx->HSSetShader(prevHs, nullptr, 0);
        ctx->DSSetShader(prevDs, nullptr, 0);
        ctx->RSSetState(prevRs);
        if (prevHs) prevHs->Release();
        if (prevDs) prevDs->Release();
        if (prevRs) prevRs->Release();
        origIB->Release();
        return true;
    }

    // Mode 1 = tess wireframe: same tess path with wireframe rasterizer.
    bool wfTess = (gControls.wireframe > 0.5f && gControls.wireframe < 1.5f);

    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
    {
        ZoneScopedN("Tess.DrawList");
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
        ZoneScopedN("Tess.DrawStrip");
        ID3D11Buffer* origIB = nullptr;
        DXGI_FORMAT   origFormat = DXGI_FORMAT_UNKNOWN;
        UINT          origOffset = 0;
        ctx->IAGetIndexBuffer(&origIB, &origFormat, &origOffset);
        if (!origIB) return false;

        ID3D11Buffer* listIB = nullptr;
        ID3D11Buffer* indirectArgs = nullptr;
        if (!GpuStripToList(ctx, origIB, origFormat, origOffset,
                            indexCount, startIndex, baseVertex,
                            &listIB, &indirectArgs))
        {
            origIB->Release();
            return false;
        }

        ctx->IASetIndexBuffer(listIB, origFormat, 0);
        bool timed = TimerBeginDraw(ctx);
        Begin(ctx);
        if (wfTess && gWireframeRs) ctx->RSSetState(gWireframeRs);
        ctx->DrawIndexedInstancedIndirect(indirectArgs, 0);
        End(ctx);
        TimerEndDraw(ctx, timed);
        ctx->IASetIndexBuffer(origIB, origFormat, origOffset);
        origIB->Release();
        return true;
    }

    return false;
}

} // namespace TerrainTess
