// SSIL bilateral blur — vertical.

Texture2D        ilTex     : register(t0);
Texture2D<float> depthTex  : register(t1);
SamplerState     samPoint  : register(s0);

cbuffer SSILParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  ilRadius;
    float  ilStrength;
    float  ilBias;
    float  ilMaxDepth;
    float  foregroundFade;
    float  falloffPower;
    float  maxScreenRadius;
    float  minScreenRadius;
    float  depthFadeStart;
    float  colorBleeding;
    float  debugMode;
    float  blurSharpness;
};

static const int BLUR_RADIUS = 4;
static const float REFERENCE_HEIGHT = 1080.0;
static const float weights[9] = {
    0.0162, 0.0540, 0.1216, 0.1836, 0.2492,
    0.1836, 0.1216, 0.0540, 0.0162
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerDepth = depthTex.Sample(samPoint, uv);
    float depthThresh = centerDepth * blurSharpness + 0.00001;
    float3 totalIL = float3(0, 0, 0);
    float totalWeight = 0.0;

    float stepScale = viewportSize.y / REFERENCE_HEIGHT;

    [unroll]
    for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++)
    {
        float2 sampleUV = uv + float2(0.0, float(i) * stepScale * invViewportSize.y);
        float3 il = ilTex.Sample(samPoint, sampleUV).rgb;
        float d = depthTex.Sample(samPoint, sampleUV);
        float w = weights[i + BLUR_RADIUS] * ((abs(d - centerDepth) < depthThresh) ? 1.0 : 0.0);
        totalIL += il * w;
        totalWeight += w;
    }

    return float4(totalIL / max(totalWeight, 0.0001), 1);
}
