// Composite: output bloom scaled by intensity (use with additive blend onto scene)

Texture2D bloomTex : register(t0);
SamplerState linearSamp : register(s0);

cbuffer BloomParams : register(b0) {
    float2 texelSize;
    float threshold;
    float softKnee;
    float intensity;
    float scatter;
    float radius;
    float padding;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 bloom = bloomTex.SampleLevel(linearSamp, uv, 0).rgb;
    return float4(bloom * intensity, 1.0);
}
