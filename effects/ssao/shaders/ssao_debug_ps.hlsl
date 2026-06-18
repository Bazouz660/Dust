// Diagnostic visualization. Each DebugView mode renders one specific
// pipeline component as ground truth so the user can identify which stage
// is broken without interpretation.
//
// Mode table (kept in sync with the debug-view registration order in
// DustSSAO.cpp; the host passes the active mode via DebugView):
//   0  Off (this shader does not run; gated CPU-side)
//   1  Final R8 AO (whiteworld view)
//   2  Raw scene depth, normalized to [0,1]
//   3  Linearized z (depth_to_z / FarPlane)
//   4  View-space position.z, normalized
//   5  Normals from depth, RGB
//   6  Per-pixel jitter value (blue noise lookup)
//   7  Filtered AO (.x from spatial filter output)

#include "ao_pipeline.hlsli"

Texture2D<float>  RawDepth   : register(t0);
Texture2D<float4> FilteredAO : register(t1);
Texture2D<float>  AoFinal    : register(t3);
SamplerState      samPoint   : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    int mode = (int)(DebugView + 0.5);

    if (mode == 1)
    {
        float ao = AoFinal.SampleLevel(samPoint, uv, 0);
        return float4(ao, ao, ao, 1);
    }

    float d = RawDepth.SampleLevel(samPoint, uv, 0);

    if (mode == 2)
        return float4(d, d, d, 1);

    float z = depth_to_z(d);

    if (mode == 3)
    {
        float n = saturate(z / max(FarPlane, 1.0));
        return float4(n, n, n, 1);
    }

    if (mode == 4)
    {
        float3 p = uv_to_proj(uv, z);
        float n = saturate(p.z / max(FarPlane, 1.0));
        return float4(n, n, n, 1);
    }

    if (mode == 5)
    {
        float edge_weight;
        float3 n = get_normals(uv, edge_weight, RawDepth, samPoint);
        return float4(n * 0.5 + 0.5, 1);
    }

    if (mode == 6)
    {
        float j = get_jitter((uint2)pos.xy);
        return float4(j, j, j, 1);
    }

    if (mode == 7)
    {
        float ao = FilteredAO.SampleLevel(samPoint, uv, 0).x;
        return float4(ao, ao, ao, 1);
    }

    return float4(0, 0, 0, 1);
}
