// Upsample: half-res AO -> full-res using hardware bilinear filtering.
// Uses depth to gate: if the full-res pixel has invalid depth, output white (no AO).
// Edge preservation is handled by the full-res bilateral blur that follows.

Texture2D<float> aoTex    : register(t0);  // Half-res raw AO (sampled with bilinear)
Texture2D<float> depthTex : register(t1);  // Full-res depth (sampled with point)
SamplerState     samLinear : register(s0);  // Bilinear clamp
SamplerState     samPoint  : register(s1);  // Point clamp

cbuffer SSAOParams : register(b0)
{
    float2 viewportSize;       // Full resolution
    float2 invViewportSize;    // 1 / full resolution
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

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerDepth = depthTex.Sample(samPoint, uv);
    if (centerDepth <= 0.0001 || centerDepth > aoMaxDepth)
        return float4(1, 1, 1, 1);

    // Hardware bilinear gives smooth interpolation of half-res AO
    float ao = aoTex.Sample(samLinear, uv);
    return float4(ao, ao, ao, 1.0);
}
