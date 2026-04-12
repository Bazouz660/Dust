#pragma once

// All bloom shaders share this constant buffer layout:
//   cbuffer BloomParams : register(b0) {
//       float2 texelSize;   // source texture texel size
//       float  threshold;   // brightness extraction threshold
//       float  intensity;   // final composite strength
//       float  radius;      // upsample kernel radius
//       float  mipWeight;   // per-mip blend weight (set per upsample pass)
//       float2 _pad;
//   };

// First-pass downsample: 13-tap Karis average + luminance prefilter
static const char* BLOOM_DOWNSAMPLE_KARIS_PS = R"(
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

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 d = texelSize;

    float3 a = srcTex.SampleLevel(linearSamp, uv + float2(-2, -2) * d, 0).rgb;
    float3 b = srcTex.SampleLevel(linearSamp, uv + float2( 0, -2) * d, 0).rgb;
    float3 c = srcTex.SampleLevel(linearSamp, uv + float2( 2, -2) * d, 0).rgb;
    float3 dd = srcTex.SampleLevel(linearSamp, uv + float2(-2,  0) * d, 0).rgb;
    float3 e = srcTex.SampleLevel(linearSamp, uv,                       0).rgb;
    float3 f = srcTex.SampleLevel(linearSamp, uv + float2( 2,  0) * d, 0).rgb;
    float3 g = srcTex.SampleLevel(linearSamp, uv + float2(-2,  2) * d, 0).rgb;
    float3 h = srcTex.SampleLevel(linearSamp, uv + float2( 0,  2) * d, 0).rgb;
    float3 i = srcTex.SampleLevel(linearSamp, uv + float2( 2,  2) * d, 0).rgb;
    float3 j = srcTex.SampleLevel(linearSamp, uv + float2(-1, -1) * d, 0).rgb;
    float3 k = srcTex.SampleLevel(linearSamp, uv + float2( 1, -1) * d, 0).rgb;
    float3 l = srcTex.SampleLevel(linearSamp, uv + float2(-1,  1) * d, 0).rgb;
    float3 m = srcTex.SampleLevel(linearSamp, uv + float2( 1,  1) * d, 0).rgb;

    float3 g0 = (a + b + dd + e) * 0.25;
    float3 g1 = (b + c + e + f) * 0.25;
    float3 g2 = (dd + e + g + h) * 0.25;
    float3 g3 = (e + f + h + i) * 0.25;
    float3 g4 = (j + k + l + m) * 0.25;

    float w0 = 1.0 / (1.0 + dot(g0, LUMA));
    float w1 = 1.0 / (1.0 + dot(g1, LUMA));
    float w2 = 1.0 / (1.0 + dot(g2, LUMA));
    float w3 = 1.0 / (1.0 + dot(g3, LUMA));
    float w4 = 1.0 / (1.0 + dot(g4, LUMA));

    float3 color = g0 * 0.125 * w0 + g1 * 0.125 * w1 +
                   g2 * 0.125 * w2 + g3 * 0.125 * w3 +
                   g4 * 0.5   * w4;
    color /= (0.125 * (w0 + w1 + w2 + w3) + 0.5 * w4);

    float brightness = dot(color, LUMA);
    float soft = brightness - threshold;
    float contribution = max(soft, 0.0) / max(brightness, 0.00001);

    return float4(color * contribution, 1.0);
}
)";

// Extract bright areas (legacy fallback)
static const char* BLOOM_EXTRACT_PS = R"(
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
    float2 d = texelSize * 0.5;
    float3 s;
    s  = srcTex.SampleLevel(linearSamp, uv + float2(-d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x, -d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2(-d.x,  d.y), 0).rgb;
    s += srcTex.SampleLevel(linearSamp, uv + float2( d.x,  d.y), 0).rgb;
    s *= 0.25;

    float brightness = max(s.r, max(s.g, s.b));
    float contribution = max(brightness - threshold, 0.0) / max(brightness, 0.00001);

    return float4(s * max(contribution, 0.0), 1.0);
}
)";

// Progressive downsample (13-tap filter)
static const char* BLOOM_DOWNSAMPLE_PS = R"(
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
    float2 d = texelSize;

    float3 a = srcTex.SampleLevel(linearSamp, uv + float2(-2, -2) * d, 0).rgb;
    float3 b = srcTex.SampleLevel(linearSamp, uv + float2( 0, -2) * d, 0).rgb;
    float3 c = srcTex.SampleLevel(linearSamp, uv + float2( 2, -2) * d, 0).rgb;
    float3 dd = srcTex.SampleLevel(linearSamp, uv + float2(-2,  0) * d, 0).rgb;
    float3 e = srcTex.SampleLevel(linearSamp, uv,                       0).rgb;
    float3 f = srcTex.SampleLevel(linearSamp, uv + float2( 2,  0) * d, 0).rgb;
    float3 g = srcTex.SampleLevel(linearSamp, uv + float2(-2,  2) * d, 0).rgb;
    float3 h = srcTex.SampleLevel(linearSamp, uv + float2( 0,  2) * d, 0).rgb;
    float3 i = srcTex.SampleLevel(linearSamp, uv + float2( 2,  2) * d, 0).rgb;
    float3 j = srcTex.SampleLevel(linearSamp, uv + float2(-1, -1) * d, 0).rgb;
    float3 k = srcTex.SampleLevel(linearSamp, uv + float2( 1, -1) * d, 0).rgb;
    float3 l = srcTex.SampleLevel(linearSamp, uv + float2(-1,  1) * d, 0).rgb;
    float3 m = srcTex.SampleLevel(linearSamp, uv + float2( 1,  1) * d, 0).rgb;

    float3 result = e * 0.125;
    result += (j + k + l + m) * 0.125;
    result += (b + dd + f + h) * 0.0625;
    result += (a + c + g + i) * 0.03125;

    return float4(result, 1.0);
}
)";

// Upsample with 9-tap tent filter
static const char* BLOOM_UPSAMPLE_PS = R"(
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
)";

// Composite: output bloom scaled by intensity (used with additive blend onto HDR)
static const char* BLOOM_COMPOSITE_PS = R"(
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
)";
