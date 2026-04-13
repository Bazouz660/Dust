// SSIL composite — additively blends indirect light onto scene.
// Used with additive blend state (ONE + ONE).

Texture2D    ilTex      : register(t0);
SamplerState samLinear  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 il = ilTex.Sample(samLinear, uv).rgb;
    return float4(il, 0.0);
}
