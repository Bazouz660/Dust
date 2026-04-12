// First-pass downsample: 13-tap with Karis average (anti-firefly) + luminance prefilter
// Replaces the old extract pass. Blooms everything proportional to luminance.

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

    // 13-tap downsample pattern (Jimenez 2014 / Call of Duty)
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

    // Group into 5 boxes for Karis weighting
    float3 g0 = (a + b + dd + e) * 0.25;
    float3 g1 = (b + c + e + f) * 0.25;
    float3 g2 = (dd + e + g + h) * 0.25;
    float3 g3 = (e + f + h + i) * 0.25;
    float3 g4 = (j + k + l + m) * 0.25;

    // Karis average: weight each group by 1/(1+luma) to suppress fireflies
    float w0 = 1.0 / (1.0 + dot(g0, LUMA));
    float w1 = 1.0 / (1.0 + dot(g1, LUMA));
    float w2 = 1.0 / (1.0 + dot(g2, LUMA));
    float w3 = 1.0 / (1.0 + dot(g3, LUMA));
    float w4 = 1.0 / (1.0 + dot(g4, LUMA));

    float3 color = g0 * 0.125 * w0 + g1 * 0.125 * w1 +
                   g2 * 0.125 * w2 + g3 * 0.125 * w3 +
                   g4 * 0.5   * w4;
    color /= (0.125 * (w0 + w1 + w2 + w3) + 0.5 * w4);

    // Soft luminance prefilter (threshold=0 means everything passes)
    float brightness = dot(color, LUMA);
    float soft = brightness - threshold;
    float contribution = max(soft, 0.0) / max(brightness, 0.00001);

    return float4(color * contribution, 1.0);
}
