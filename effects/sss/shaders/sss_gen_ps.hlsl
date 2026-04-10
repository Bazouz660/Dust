// Screen Space Shadows — ray marches toward the sun in view space using the depth buffer.
// Outputs a shadow mask: white (1.0) = lit, dark (0.0) = shadowed.

Texture2D<float> depthTex   : register(t0);
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

static const float PI = 3.14159265;

float3 ReconstructViewPos(float2 uv, float depth)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * aspectRatio * tanHalfFov * depth;
    pos.y = (1.0 - uv.y * 2.0) * tanHalfFov * depth;
    pos.z = depth;
    return pos;
}

float2 ViewPosToUV(float3 viewPos)
{
    float2 uv;
    uv.x = (viewPos.x / (viewPos.z * aspectRatio * tanHalfFov) + 1.0) * 0.5;
    uv.y = (1.0 - viewPos.y / (viewPos.z * tanHalfFov)) * 0.5;
    return uv;
}

// Interleaved gradient noise for per-pixel jitter
float InterleavedGradientNoise(float2 screenPos)
{
    return frac(52.9829189 * frac(0.06711056 * screenPos.x + 0.00583715 * screenPos.y));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, uv);

    // Skip sky and distant pixels
    if (depth <= 0.0001 || depth > maxDepth)
        return float4(1, 1, 1, 1);

    float3 viewPos = ReconstructViewPos(uv, depth);
    float3 lightDir = normalize(sunDirView);

    // Per-pixel jitter to break banding
    float noise = InterleavedGradientNoise(pos.xy);

    float shadow = 0.0;
    int numSteps = (int)stepCount;

    [loop]
    for (int i = 1; i <= numSteps; i++)
    {
        // Quadratic step distribution — more samples near the pixel for fine contact shadows
        float t = (float(i) + noise * 0.5) / float(numSteps);
        float dist = t * t * maxDistance;

        float3 marchPos = viewPos + lightDir * dist;

        // Stop if behind camera
        if (marchPos.z <= 0.001)
            break;

        float2 marchUV = ViewPosToUV(marchPos);

        // Stop if outside screen
        if (any(marchUV < 0.0) || any(marchUV > 1.0))
            break;

        float sampleDepth = depthTex.Sample(pointClamp, marchUV);

        // How much deeper is our march point than the scene at that screen position?
        float depthDiff = marchPos.z - sampleDepth;

        // Occluded: scene is closer to camera than our march position (depthDiff > 0)
        // but not by too much (thickness check prevents self-shadowing from distant surfaces)
        if (depthDiff > depthBias && depthDiff < thickness)
        {
            // Smooth falloff: closer occluders cast stronger shadows
            float occStrength = saturate(1.0 - t);
            shadow = max(shadow, occStrength);
        }
    }

    float result = 1.0 - shadow * strength;
    return float4(result, result, result, 1.0);
}
