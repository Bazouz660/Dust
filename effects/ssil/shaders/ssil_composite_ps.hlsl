// SSIL composite — additively blends indirect light onto scene.
// Used with additive blend state (ONE + ONE).

Texture2D    ilTex      : register(t0);
SamplerState samLinear  : register(s0);

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

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 il = ilTex.Sample(samLinear, uv).rgb;
    return float4(il, 1.0);
}
