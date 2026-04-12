// Extract bright areas from HDR scene + downsample to half res (legacy fallback)

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
    // 4-tap bilinear downsample (hardware bilinear gives effective 4x4 coverage)
    float2 d = texelSize * 0.5;
    float3 s;
    s  = srcTex.SampleLevel(linearSamp, uv + float2(-d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2(-d.x,  d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x,  d.y), 0).rgb;
    s *= 0.25;

    // Soft brightness threshold
    float brightness = max(s.r, max(s.g, s.b));
    float contribution = max(brightness - threshold, 0.0) / max(brightness, 0.00001);

    return float4(s * max(contribution, 0.0), 1.0);
}
