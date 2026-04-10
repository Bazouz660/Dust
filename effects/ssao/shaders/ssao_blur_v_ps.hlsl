// Bilateral blur — vertical.

Texture2D<float> aoTex     : register(t0);
Texture2D<float> depthTex  : register(t1);
SamplerState     samPoint  : register(s0);

cbuffer SSAOParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  filterRadius;
    float  debugMode;
    float  aoRadius;
    float  aoStrength;
    float  aoBias;
    float  aoMaxDepth;
    float  foregroundFade;
    float  falloffPower;
    float  maxScreenRadius;
    float  minScreenRadius;
    float  depthFadeStart;
    float  blurSharpness;
};

static const int BLUR_RADIUS = 6;
static const float REFERENCE_HEIGHT = 1080.0;
static const float weights[13] = {
    0.0044, 0.0115, 0.0257, 0.0488, 0.0799, 0.1122, 0.1350,
    0.1122, 0.0799, 0.0488, 0.0257, 0.0115, 0.0044
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerDepth = depthTex.Sample(samPoint, uv);
    float depthThresh = centerDepth * blurSharpness + 0.00001;
    float totalAO = 0.0, totalWeight = 0.0;

    float stepScale = viewportSize.y / REFERENCE_HEIGHT;

    [unroll]
    for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++)
    {
        float2 sampleUV = uv + float2(0.0, float(i) * stepScale * invViewportSize.y);
        float ao = aoTex.Sample(samPoint, sampleUV);
        float d = depthTex.Sample(samPoint, sampleUV);
        float w = weights[i + BLUR_RADIUS] * ((abs(d - centerDepth) < depthThresh) ? 1.0 : 0.0);
        totalAO += ao * w;
        totalWeight += w;
    }

    return float4(totalAO / max(totalWeight, 0.0001), 0, 0, 1);
}
