// Circle of Confusion (CoC) generation from linear depth.
// Output: R16_FLOAT, 0 = in focus, 1 = maximum blur.

Texture2D<float> depthTex   : register(t0);
SamplerState     pointClamp : register(s0);

cbuffer DOFParams : register(b0)
{
    float2 texelSize;
    float  focusDistance;
    float  focusRange;
    float  blurStrength;
    float  blurRadius;
    float  maxDepth;
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, uv);

    // Sky and distant pixels: no blur
    if (depth <= 0.0001 || depth > maxDepth)
        return float4(0, 0, 0, 1);

    // Distance from focus plane
    float dist = abs(depth - focusDistance);

    // Smooth transition from sharp to blurred
    float coc = smoothstep(0.0, focusRange, dist);

    // Scale by blur strength
    coc = saturate(coc * blurStrength);

    return float4(coc, coc, coc, 1);
}
