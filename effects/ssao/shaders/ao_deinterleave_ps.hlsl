// Depth deinterleave pass.
// Reads Kenshi's linearized depth, applies depth_to_z, and stores it
// at the deinterleaved position so subsequent AO samples are cache-coherent
// within each tile.

#include "ao_pipeline.hlsli"

Texture2D<float> DepthInput : register(t0);
SamplerState     PointSamp  : register(s0);

float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 get_uv = deinterleave_uv(uv);
    float linear_depth = DepthInput.SampleLevel(PointSamp, get_uv, 0);
    return depth_to_z(linear_depth);
}
