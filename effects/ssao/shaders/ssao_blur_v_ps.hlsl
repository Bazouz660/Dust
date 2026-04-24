// Guided filter blur — pass 2 (negative gather offsets).
// Depth-guided local linear model for smooth edge-preserving filtering.

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
    float  noiseScale;
    float  numDirections;
    float  numSteps;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerDepth = depthTex.Sample(samPoint, uv);

    // Accumulate moments over 4x4 neighborhood via Gather4
    // Negated offsets shift the window relative to pass 1
    float4 mv = 0;
    float4 ao, depth;

    ao    = aoTex.Gather(samPoint, uv + float2(0.5, 0.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(0.5, 0.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(-1.5, 0.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(-1.5, 0.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(0.5, -1.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(0.5, -1.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(-1.5, -1.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(-1.5, -1.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    mv /= 16.0;

    // Guided filter: ao = a + b * depth
    float depth_var  = mv.y - mv.x * mv.x;
    float covariance = mv.w - mv.x * mv.z;

    // Smooth falloff: variance-adaptive regularization
    float relVar = depth_var / max(centerDepth * centerDepth, 1e-10);
    float epsilon = exp2(relVar > 0.01 ? -12.0 : -30.0);

    float b = covariance / max(depth_var, epsilon);
    float a = mv.z - b * mv.x;

    return float4(saturate(a + b * centerDepth), 0, 0, 1);
}
