// Vertical Gaussian blur for DOF at half resolution.
// Uses bilinear filtering trick: sample between texel pairs to get 2-for-1 taps.

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
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float sigma = max(blurRadius / 3.0, 0.5);
    float invSigma2 = -0.5 / (sigma * sigma);

    int iRadius = (int)ceil(blurRadius);
    iRadius = min(iRadius, 32);

    // Center tap
    float w0 = 1.0; // exp(0) = 1
    float3 total = sceneTex.SampleLevel(linearClamp, uv, 0).rgb * w0;
    float totalWeight = w0;

    // Pair taps using bilinear trick
    [loop]
    for (int i = 1; i <= iRadius; i += 2)
    {
        float w1 = exp(float(i * i) * invSigma2);
        float w2 = (i + 1 <= iRadius) ? exp(float((i+1) * (i+1)) * invSigma2) : 0.0;
        float wSum = w1 + w2;
        if (wSum < 0.0001) break;

        float offset = (float(i) * w1 + float(i + 1) * w2) / wSum;

        float2 uvOff = float2(0.0, offset * texelSize.y);
        total += sceneTex.SampleLevel(linearClamp, uv + uvOff, 0).rgb * wSum;
        total += sceneTex.SampleLevel(linearClamp, uv - uvOff, 0).rgb * wSum;
        totalWeight += wSum * 2.0;
    }

    return float4(total / totalWeight, 1.0);
}
