// Horizontal Gaussian blur for DOF at half resolution.
// Uses bilinear filtering trick: sample between texel pairs to get 2-for-1 taps.
// Brightness-weighted: bright samples get higher weight, creating star-like streaks.

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
    float sigma = max(blurRadius / 3.0, 0.5);
    float invSigma2 = -0.5 / (sigma * sigma);

    int iRadius = (int)ceil(blurRadius);
    iRadius = min(iRadius, 32);

    float3 center = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float centerB = max(center.r, max(center.g, center.b));
    float w0 = 1.0 + max(0.0, centerB - highlightThreshold) * highlightBoost;
    float3 total = center * w0;
    float totalWeight = w0;

    [loop]
    for (int i = 1; i <= iRadius; i += 2)
    {
        float w1 = exp(float(i * i) * invSigma2);
        float w2 = (i + 1 <= iRadius) ? exp(float((i+1) * (i+1)) * invSigma2) : 0.0;
        float wSum = w1 + w2;
        if (wSum < 0.0001) break;

        float offset = (float(i) * w1 + float(i + 1) * w2) / wSum;

        float2 uvOff = float2(offset * texelSize.x, 0.0);
        float3 sPlus = sceneTex.SampleLevel(linearClamp, uv + uvOff, 0).rgb;
        float3 sMinus = sceneTex.SampleLevel(linearClamp, uv - uvOff, 0).rgb;

        float bPlus = max(sPlus.r, max(sPlus.g, sPlus.b));
        float bMinus = max(sMinus.r, max(sMinus.g, sMinus.b));

        float wPlus = wSum * (1.0 + max(0.0, bPlus - highlightThreshold) * highlightBoost);
        float wMinus = wSum * (1.0 + max(0.0, bMinus - highlightThreshold) * highlightBoost);

        total += sPlus * wPlus + sMinus * wMinus;
        totalWeight += wPlus + wMinus;
    }

    return float4(total / totalWeight, 1.0);
}
