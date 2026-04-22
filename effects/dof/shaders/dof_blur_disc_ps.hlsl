// Disc blur - circular bokeh shape (replaces separable Gaussian)
// Single-pass gather using golden angle spiral sampling

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

    float3 total = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float totalWeight = 1.0;

    [loop]
    for (int i = 1; i <= sampleCount; i++)
    {
        float t = float(i) / float(sampleCount + 1);
        float r = sqrt(t) * blurRadius;
        float angle = float(i) * GOLDEN_ANGLE;

        float2 offset = float2(cos(angle), sin(angle)) * r * texelSize;

        total += sceneTex.SampleLevel(linearClamp, uv + offset, 0).rgb;
        totalWeight += 1.0;
    }

    return float4(total / totalWeight, 1.0);
}
