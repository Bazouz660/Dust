// 4-tap box downsample for DOF blur prefilter.
// Brightness-weighted: bright pixels (stars, lights) dominate so they
// survive downsampling and produce visible bokeh in the blur pass.

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

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 d = texelSize * 0.5;

    float3 s0 = sceneTex.SampleLevel(linearClamp, uv + float2(-d.x, -d.y), 0).rgb;
    float3 s1 = sceneTex.SampleLevel(linearClamp, uv + float2( d.x, -d.y), 0).rgb;
    float3 s2 = sceneTex.SampleLevel(linearClamp, uv + float2(-d.x,  d.y), 0).rgb;
    float3 s3 = sceneTex.SampleLevel(linearClamp, uv + float2( d.x,  d.y), 0).rgb;

    float b0 = max(s0.r, max(s0.g, s0.b));
    float b1 = max(s1.r, max(s1.g, s1.b));
    float b2 = max(s2.r, max(s2.g, s2.b));
    float b3 = max(s3.r, max(s3.g, s3.b));

    float w0 = 1.0 + max(0.0, b0 - highlightThreshold) * highlightBoost;
    float w1 = 1.0 + max(0.0, b1 - highlightThreshold) * highlightBoost;
    float w2 = 1.0 + max(0.0, b2 - highlightThreshold) * highlightBoost;
    float w3 = 1.0 + max(0.0, b3 - highlightThreshold) * highlightBoost;

    float totalW = w0 + w1 + w2 + w3;
    return float4((s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3) / totalW, 1.0);
}
