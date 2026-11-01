// Dust SSAO: XeGTAO (Intel, MIT; see xegtao.hlsli) adapted to Kenshi's G-buffer.
//
// Passes (compile each entry point separately, cs_5_0):
//   CSPrefilterDepths16x16  radial depth01 -> view-space Z (depth01 units), 5-level MIP chain
//   CSMain                  GTAO horizon search per slice; smooth normals generated in place from the
//                           corrected depth; normal-map detail added afterwards as a cavity term
//   CSDenoise / CSDenoiseLast  XeGTAO edge-aware 3x3 denoiser; the last pass writes the final AO
//
// Why not the previous shader: it reconstructed positions with z = radial depth (bending every flat
// surface by up to ~20 degrees against the true G-buffer normal), measured horizons against the
// normal-MAPPED normal (every texture bump tilted the reference plane), and its blur averaged AO
// across depth edges. Each produced its own kind of halo.

#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0   // Dust radii are ~0.003 depth01 units: below fp16 precision near 1.0
#define XE_GTAO_FP32_DEPTHS
#define XE_GTAO_GENERATE_NORMALS_INPLACE     // smooth geometric normals from the corrected depth
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#define VA_SATURATE saturate               // from the XeGTAO sample framework (vaShared.hlsl)

#include "xegtao.h"

cbuffer GTAOConstantBuffer : register(b0)
{
    GTAOConstants g_consts;
}

cbuffer DustAOParams : register(b1)
{
    float4 dustCamRight;
    float4 dustCamUp;
    float4 dustCamForward;
    float  dustNormalDetail;      // 0 = no normal-map detail, 1 = full cavity term
    float  dustMaxRadial;         // AO only up to this radial depth01 (MaxDepth)
    float  dustFadeStart;         // fade AO out from here to dustMaxRadial (DepthFadeStart)
    float  dustMaxScreenRadiusPx; // Max Screen Radius, in pixels
    float  dustSliceCount;
    float  dustStepsPerSlice;
    float  dustPad0;
    float  dustPad1;
}

#define XE_GTAO_DUST_MAX_SCREEN_RADIUS dustMaxScreenRadiusPx

SamplerState g_pointClamp : register(s0);

// Resources read by DustPostVisibility (declared before the XeGTAO include, which calls it).
Texture2D<float4>  g_srcGBufferNormals   : register(t1);   // main pass: world normals, *2-1 (normal-mapped)
Texture2D<float>   g_srcRadialDepth      : register(t2);   // main pass: raw radial depth (MaxDepth / fade / sky)

// Normal-map detail, depth range and fade, applied to the raw visibility before denoising. The horizon
// search itself only ever sees the smooth normal: a texture bump must not tilt the reference plane.
float DustPostVisibility(uint2 pix, float visibility, float3 smoothNormal, float viewZ)
{
    const float radial = g_srcRadialDepth.Load(int3(pix, 0));
    if (radial <= 0.0001 || radial > dustMaxRadial)
        return 1.0;

    [branch]
    if (dustNormalDetail > 0.0)
    {
        float3 w = g_srcGBufferNormals.Load(int3(pix, 0)).rgb * 2.0 - 1.0;
        float3 v = float3(dot(w, dustCamRight.xyz), dot(w, dustCamUp.xyz), -dot(w, dustCamForward.xyz));
        const float lenSq = dot(v, v);
        if (lenSq > 1e-4)
        {
            // Bumps that turn away from the underlying surface are partly hidden from the sky.
            const float cavity = saturate(dot(v * rsqrt(lenSq), smoothNormal));
            visibility *= lerp(1.0, cavity * cavity, dustNormalDetail);
        }
    }

    const float fade = saturate(1.0 - max(radial - dustFadeStart, 0.0) / max(dustMaxRadial - dustFadeStart, 1e-4));
    return lerp(1.0, visibility, fade);
}
#define XE_GTAO_DUST_POST_VISIBILITY(pix, vis, n, z) DustPostVisibility(pix, vis, n, z)

#include "xegtao.hlsli"

// Resources passed to XeGTAO functions: declared with its lpfloat typedef (FXC won't convert
// RWTexture2D<float> to RWTexture2D<lpfloat> even when lpfloat is float).
// Prefilter pass
Texture2D<float>     g_srcRawDepth         : register(t0);   // Kenshi radial depth01 (R32F)
RWTexture2D<lpfloat> g_outWorkingDepthMIP0 : register(u0);
RWTexture2D<lpfloat> g_outWorkingDepthMIP1 : register(u1);
RWTexture2D<lpfloat> g_outWorkingDepthMIP2 : register(u2);
RWTexture2D<lpfloat> g_outWorkingDepthMIP3 : register(u3);
RWTexture2D<lpfloat> g_outWorkingDepthMIP4 : register(u4);
// Main pass
Texture2D<lpfloat>   g_srcWorkingDepth     : register(t0);   // view Z MIP chain from the prefilter
RWTexture2D<uint>    g_outWorkingAOTerm    : register(u0);
RWTexture2D<unorm float> g_outWorkingEdges : register(u1);
// Denoise passes
Texture2D<uint>      g_srcWorkingAOTerm    : register(t0);
Texture2D<lpfloat>   g_srcWorkingEdges     : register(t1);
RWTexture2D<uint>    g_outFinalAOTerm      : register(u0);   // R8_UINT view of the R8_TYPELESS AO texture

[numthreads(8, 8, 1)]   // each thread does 2x2 pixels: dispatch (w+15)/16, (h+15)/16
void CSPrefilterDepths16x16(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_consts, g_srcRawDepth, g_pointClamp,
        g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2, g_outWorkingDepthMIP3, g_outWorkingDepthMIP4);
}

// Hilbert-curve-driven R2 sequence (XeGTAO sample). No TAA in general: temporal index 0.
float2 DustSpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = HilbertIndex(pixCoord.x, pixCoord.y);
    index += 288 * (temporalIndex % 64);
    return frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114));
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]   // dispatch (w+7)/8, (h+7)/8
void CSMain(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, dustSliceCount, dustStepsPerSlice, DustSpatioTemporalNoise(pixCoord, g_consts.NoiseIndex),
        float3(0, 0, 0), g_consts, g_srcWorkingDepth, g_pointClamp, g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]   // 2 pixels per thread: dispatch (w+15)/16, (h+7)/8
void CSDenoise(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_consts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_pointClamp, g_outFinalAOTerm, false);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoiseLast(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_consts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_pointClamp, g_outFinalAOTerm, true);
}
