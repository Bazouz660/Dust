// Upsample with 9-tap tent filter (used with additive blending onto larger mip)

Texture2D srcTex : register(t0);
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
    float2 d = texelSize * radius;

    // 3x3 tent filter (weights: 1 2 1 / 2 4 2 / 1 2 1, sum = 16)
    float3 s;
    s  = srcTex.SampleLevel(linearSamp, uv + float2(-d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( 0.0, -d.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2(-d.x,  0.0), 0).rgb * 2.0;
    s += srcTex.SampleLevel(linearSamp, uv,                       0).rgb * 4.0;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x,  0.0), 0).rgb * 2.0;
    s += srcTex.SampleLevel(linearSamp, uv + float2(-d.x,  d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( 0.0,  d.y), 0).rgb * 2.0;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x,  d.y), 0).rgb;
    s /= 16.0;

    return float4(s * mipWeight, 1.0);
}
