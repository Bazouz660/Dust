// Circle of Confusion (CoC) generation from linear depth.
// Output: R16_FLOAT
//   Positive = far field blur (behind focus)
//   Negative = near field blur (in front of focus)
//   0 = in focus

Texture2D<float> depthTex   : register(t0);
SamplerState     pointClamp : register(s0);

cbuffer DOFParams : register(b0)
{
    float2 texelSize;
    float  focusDistance;   // Resolved focus distance (auto or manual)
    float  nearStart;      // Near blur begins at this distance from focus
    float  nearEnd;        // Near blur reaches max at this distance from focus
    float  nearStrength;
    float  farStart;       // Far blur begins at this distance from focus
    float  farEnd;         // Far blur reaches max at this distance from focus
    float  farStrength;
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

    float diff = depth - focusDistance;
    float coc = 0.0;

    if (diff > 0.0)
    {
        // Far field (behind focus plane)
        coc = smoothstep(farStart, farEnd, diff) * farStrength;
    }
    else
    {
        // Near field (in front of focus plane)
        float absDiff = -diff;
        coc = -smoothstep(nearStart, nearEnd, absDiff) * nearStrength;
    }

    return float4(coc, coc, coc, 1);
}
