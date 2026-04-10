#pragma once

// All bloom shaders share this constant buffer layout:
//   cbuffer BloomParams : register(b0) {
//       float2 texelSize;   // source texture texel size
//       float  threshold;   // brightness extraction threshold
//       float  softKnee;    // soft threshold transition
//       float  intensity;   // final composite strength
//       float  scatter;     // upsample contribution weight
//       float2 padding;
//   };

// Extract bright areas from HDR scene + downsample to half res
static const char* BLOOM_EXTRACT_PS = R"(
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
    // 4-tap bilinear downsample (hardware bilinear gives effective 4x4 coverage)
    float2 d = texelSize * 0.5;
    float3 s;
    s  = srcTex.SampleLevel(linearSamp, uv + float2(-d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2(-d.x,  d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x,  d.y), 0).rgb;
    s *= 0.25;

    // Soft brightness threshold (smooth knee curve)
    float brightness = max(s.r, max(s.g, s.b));
    float knee = threshold * softKnee;
    float soft = brightness - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    float contribution = max(soft, brightness - threshold) / max(brightness, 0.00001);

    return float4(s * max(contribution, 0.0), 1.0);
}
)";

// Progressive downsample (4-tap bilinear box filter)
static const char* BLOOM_DOWNSAMPLE_PS = R"(
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
)";

// Upsample with 9-tap tent filter (used with additive blending onto larger mip)
static const char* BLOOM_UPSAMPLE_PS = R"(
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
    float2 d = texelSize;

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

    return float4(s * scatter, 1.0);
}
)";

// Composite: output bloom scaled by intensity (use with additive blend onto scene)
static const char* BLOOM_COMPOSITE_PS = R"(
Texture2D bloomTex : register(t0);
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
    float3 bloom = bloomTex.SampleLevel(linearSamp, uv, 0).rgb;
    return float4(bloom * intensity, 1.0);
}
)";
