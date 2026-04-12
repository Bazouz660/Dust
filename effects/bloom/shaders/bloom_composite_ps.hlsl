// Composite: output bloom scaled by intensity (used with additive blend onto HDR)

Texture2D bloomTex : register(t0);
SamplerState linearSamp : register(s0);

cbuffer BloomParams : register(b0) {
    float2 texelSize;
    float threshold;
    float intensity;
    float radius;
    float mipWeight;
    float2 _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 bloom = bloomTex.SampleLevel(linearSamp, uv, 0).rgb;
    return float4(bloom * intensity, 1.0);
}
