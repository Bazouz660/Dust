// Bilateral vertical blur for Screen Space Shadows.
// Preserves depth edges to avoid shadow bleeding across objects.

Texture2D<float> sssTex     : register(t0);
Texture2D<float> depthTex   : register(t1);
SamplerState     pointClamp : register(s0);

cbuffer SSSParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  maxDistance;
    float  thickness;
    float3 sunDirView;
    float  strength;
    float  stepCount;
    float  maxDepth;
    float  depthBias;
    float  blurSharpness;
};

static const int BLUR_RADIUS = 4;

// Gaussian weights for radius 4
static const float weights[5] = { 0.2270, 0.1945, 0.1216, 0.0541, 0.0162 };

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerSSS   = sssTex.Sample(pointClamp, uv);
    float centerDepth = depthTex.Sample(pointClamp, uv);

    if (centerDepth <= 0.0001)
        return float4(centerSSS, centerSSS, centerSSS, 1.0);

    float totalWeight = weights[0];
    float totalSSS    = centerSSS * weights[0];

    [unroll]
    for (int i = 1; i <= BLUR_RADIUS; i++)
    {
        float2 offset = float2(0.0, float(i) * invViewportSize.y);

        // Positive direction
        {
            float2 sampleUV = uv + offset;
            float sSSS   = sssTex.Sample(pointClamp, sampleUV);
            float sDepth = depthTex.Sample(pointClamp, sampleUV);
            float depthDiff = abs(sDepth - centerDepth);
            float bilateralWeight = exp(-depthDiff / (blurSharpness + 0.0001));
            float w = weights[i] * bilateralWeight;
            totalSSS += sSSS * w;
            totalWeight += w;
        }

        // Negative direction
        {
            float2 sampleUV = uv - offset;
            float sSSS   = sssTex.Sample(pointClamp, sampleUV);
            float sDepth = depthTex.Sample(pointClamp, sampleUV);
            float depthDiff = abs(sDepth - centerDepth);
            float bilateralWeight = exp(-depthDiff / (blurSharpness + 0.0001));
            float w = weights[i] * bilateralWeight;
            totalSSS += sSSS * w;
            totalWeight += w;
        }
    }

    float result = totalSSS / totalWeight;
    return float4(result, result, result, 1.0);
}
