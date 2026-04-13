// RTGI Composite Pass — applies GI intensity scaling at the final stage.
// Drawn with additive blend onto the HDR scene.

Texture2D<float4> giTex  : register(t0);
SamplerState linearClamp : register(s0);

cbuffer CompositeParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  giIntensity;
    float  saturation;
    float  _pad0;
    float  _pad1;
};

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 gi = giTex.SampleLevel(linearClamp, uv, 0);

    float3 indirect = gi.rgb * giIntensity;

    // Saturation control
    float lum = Luminance(indirect);
    indirect = lerp(float3(lum, lum, lum), indirect, saturation);

    return float4(indirect, 1.0);
}
