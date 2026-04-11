// Vertical Gaussian blur for DOF at half resolution.

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

    float3 total = float3(0, 0, 0);
    float totalWeight = 0.0;

    int iRadius = (int)ceil(blurRadius);
    iRadius = min(iRadius, 32);

    [loop]
    for (int i = -iRadius; i <= iRadius; i++)
    {
        float w = exp(float(i * i) * invSigma2);
        float2 sampleUV = uv + float2(0.0, float(i) * texelSize.y);
        total += sceneTex.SampleLevel(linearClamp, sampleUV, 0).rgb * w;
        totalWeight += w;
    }

    return float4(total / totalWeight, 1.0);
}
