// Final resolve: read .x (AO) from the RGBA16F source and output to the R8
// AO target consumed by Kenshi's deferred shader at slot 8.

#include "ao_pipeline.hlsli"

Texture2D<float4> Source   : register(t0);
SamplerState      PointSamp : register(s0);

float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return Source.SampleLevel(PointSamp, uv, 0).x;
}
