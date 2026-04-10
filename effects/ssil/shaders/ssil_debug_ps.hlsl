// SSIL debug — no-blend pass. Overwrites left third with raw indirect light visualization.

Texture2D    ilTex     : register(t0);
SamplerState samLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    if (uv.x >= 0.333)
        discard;

    float3 il = ilTex.Sample(samLinear, uv).rgb;
    // Scale into visible range for auto-exposure
    float3 v = il * 0.05;
    return float4(v, 1.0);
}
