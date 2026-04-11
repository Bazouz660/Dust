// AO apply — reads scene copy + AO, simple multiply blend.

Texture2D<float>  aoTex    : register(t0);
Texture2D         sceneTex : register(t1);
SamplerState      samPoint : register(s0);

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

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float ao = aoTex.Sample(samPoint, uv);
    float3 scene = sceneTex.Sample(samPoint, uv).rgb;

    // In debug mode: only apply AO to middle third, pass through rest
    if (debugMode > 0.5)
    {
        if (uv.x < 0.333 || uv.x > 0.666)
            return float4(scene, 1);
    }

    float3 result = scene * ao;
    return float4(result, 1.0);
}
