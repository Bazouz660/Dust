// Vertical Gaussian blur for Clarity local contrast extraction.
// Large-radius separable blur to isolate low-frequency content.

Texture2D sceneTex       : register(t0);
SamplerState linearClamp : register(s0);

cbuffer ClarityParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  strength;
    float  midtoneProtect;
    float  blurRadius;
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float sigma = max(blurRadius / 3.0, 0.5);
    float invSigma2 = -0.5 / (sigma * sigma);

    float3 total = float3(0, 0, 0);
    float totalWeight = 0.0;

    int iRadius = (int)ceil(blurRadius);
    iRadius = min(iRadius, 32);

    [loop]
    for (int i = -iRadius; i <= iRadius; i++)
    {
        float w = exp(float(i * i) * invSigma2);
        float2 sampleUV = uv + float2(0.0, float(i) * invViewportSize.y);
        total += sceneTex.SampleLevel(linearClamp, sampleUV, 0).rgb * w;
        totalWeight += w;
    }

    float3 result = total / totalWeight;
    return float4(result, 1.0);
}
