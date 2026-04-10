// Debug visualization for Screen Space Shadows.
// Renders the raw shadow mask (white = lit, black = shadowed).

Texture2D<float> sssTex     : register(t0);
SamplerState     pointClamp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float shadow = sssTex.Sample(pointClamp, uv);
    return float4(shadow, shadow, shadow, 1.0);
}
