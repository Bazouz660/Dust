// Progressive downsample (4-tap bilinear box filter)

Texture2D srcTex : register(t0);
SamplerState linearSamp : register(s0);

cbuffer BloomParams : register(b0) {
    float2 texelSize;
    float threshold;
    float softKnee;
    float intensity;
    float scatter;
    float2 padding;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 d = texelSize * 0.5;
    float3 s;
    s  = srcTex.SampleLevel(linearSamp, uv + float2(-d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2(-d.x,  d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x,  d.y), 0).rgb;
    return float4(s * 0.25, 1.0);
}
