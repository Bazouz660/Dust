// dpm_shadow_debug_ps.hlsl — fullscreen debug view of the ray-traced shadow mask.
// White = lit, black = shadowed. Used to validate DPM ray test quality.

Texture2D<float> g_ShadowMask : register(t0);
SamplerState     g_Samp       : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float shadow = g_ShadowMask.SampleLevel(g_Samp, uv, 0);
    return float4(shadow, shadow, shadow, 1.0);
}
