// Disc blur - circular bokeh shape (replaces separable Gaussian)
// Single-pass gather using golden angle spiral sampling.
// Brightness-weighted: bright samples dominate, creating visible bokeh discs.

Texture2D sceneTex       : register(t0);
SamplerState linearClamp : register(s0);

cbuffer DOFParams : register(b0)
{
    float2 texelSize;
    float  focusDistance;
    float  nearStart;
    float  nearEnd;
    float  nearStrength;
    float  farStart;
    float  farEnd;
    float  farStrength;
    float  blurRadius;
    float  maxDepth;
    int    cocMode;
    float  aperture;
    float  highlightThreshold;
    float  highlightBoost;
    float  _pad;
};

static const float GOLDEN_ANGLE = 2.39996323;

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    int sampleCount = clamp((int)(blurRadius * 8.0), 8, 64);

    float3 center = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float centerB = max(center.r, max(center.g, center.b));
    float centerW = 1.0 + max(0.0, centerB - highlightThreshold) * highlightBoost;
    float3 total = center * centerW;
    float totalWeight = centerW;

    [loop]
    for (int i = 1; i <= sampleCount; i++)
    {
        float t = float(i) / float(sampleCount + 1);
        float r = sqrt(t) * blurRadius;
        float angle = float(i) * GOLDEN_ANGLE;

        float2 offset = float2(cos(angle), sin(angle)) * r * texelSize;

        float3 s = sceneTex.SampleLevel(linearClamp, uv + offset, 0).rgb;
        float b = max(s.r, max(s.g, s.b));
        float w = 1.0 + max(0.0, b - highlightThreshold) * highlightBoost;
        total += s * w;
        totalWeight += w;
    }

    return float4(total / totalWeight, 1.0);
}
