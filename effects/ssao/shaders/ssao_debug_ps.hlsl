// AO debug — no-blend pass. Overwrites left third with raw AO visualization.
// White = unoccluded, dark = occluded.

Texture2D<float> aoTex     : register(t0);
SamplerState     samLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    if (uv.x >= 0.333)
        discard;

    float ao = aoTex.Sample(samLinear, uv);
    return float4(ao, ao, ao, 1.0);
}
