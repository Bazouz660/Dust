// sss_cs.hlsl - Bend Studio Screen Space Shadows compute-shader wrapper (Dust).
//
// Declares the resources the FXC-ported bend_sss_gpu.h expects as globals
// (gSSSDepth / gSSSOut / gSSSSamp), fills a DispatchParameters struct from the
// per-dispatch constant buffer, then calls WriteScreenSpaceShadow().
//
// cbuffer field order/packing MUST match SSSCBData in DustSSS.cpp (16-byte rows).

Texture2D<float>   gSSSDepth : register(t0);
RWTexture2D<float> gSSSOut   : register(u0);
SamplerState       gSSSSamp  : register(s0);

cbuffer SSSCB : register(b0)
{
    float4 cbLightCoordinate;        // c0
    int2   cbWaveOffset;             // c1.xy
    float2 cbInvDepthTextureSize;    // c1.zw
    float  cbSurfaceThickness;       // c2.x
    float  cbBilinearThreshold;      // c2.y
    float  cbShadowContrast;         // c2.z
    float  cbFarDepthValue;          // c2.w
    float  cbNearDepthValue;         // c3.x
    float2 cbDepthBounds;            // c3.yz
    float  cbPad0;                   // c3.w
    int    cbIgnoreEdgePixels;       // c4.x
    int    cbUsePrecisionOffset;     // c4.y
    int    cbBilinearSamplingOffsetMode; // c4.z
    int    cbDebugOutputEdgeMask;    // c4.w
    int    cbDebugOutputThreadIndex; // c5.x
    int    cbDebugOutputWaveIndex;   // c5.y
    int    cbPad1;                   // c5.z
    int    cbPad2;                   // c5.w
};

#define WAVE_SIZE 64
#define SAMPLE_COUNT 60
#define HARD_SHADOW_SAMPLES 4
#define FADE_OUT_SAMPLES 8

#include "bend_sss_gpu.hlsli"

[numthreads(WAVE_SIZE, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gtid : SV_GroupThreadID)
{
    DispatchParameters p;
    p.SurfaceThickness           = cbSurfaceThickness;
    p.BilinearThreshold          = cbBilinearThreshold;
    p.ShadowContrast             = cbShadowContrast;
    p.IgnoreEdgePixels           = cbIgnoreEdgePixels != 0;
    p.UsePrecisionOffset         = cbUsePrecisionOffset != 0;
    p.BilinearSamplingOffsetMode = cbBilinearSamplingOffsetMode != 0;
    p.DebugOutputEdgeMask        = cbDebugOutputEdgeMask != 0;
    p.DebugOutputThreadIndex     = cbDebugOutputThreadIndex != 0;
    p.DebugOutputWaveIndex       = cbDebugOutputWaveIndex != 0;
    p.DepthBounds                = cbDepthBounds;
    p.UseEarlyOut                = false;   // early-out path removed for FXC cs_5_0
    p.LightCoordinate            = cbLightCoordinate;
    p.WaveOffset                 = cbWaveOffset;
    p.FarDepthValue              = cbFarDepthValue;
    p.NearDepthValue             = cbNearDepthValue;
    p.InvDepthTextureSize        = cbInvDepthTextureSize;

    WriteScreenSpaceShadow(p, (int3)gid, (int)gtid);
}
