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
    float  focusDistance;
    float  nearStart;
    float  nearEnd;
    float  nearStrength;
    float  farStart;
    float  farEnd;
    float  farStrength;
    float  blurRadius;
    float  maxDepth;
    int    cocMode;
    float  aperture;
    float  highlightThreshold;
    float  highlightBoost;
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, uv);

    if (depth <= 0.0001 || depth > maxDepth)
        depth = maxDepth;

    float coc = 0.0;

    if (cocMode > 0)
    {
        // Physical thin-lens: CoC proportional to |1/focus - 1/depth|
        float rawCoC = aperture * (1.0 / focusDistance - 1.0 / depth);
        coc = (rawCoC > 0.0) ? min(rawCoC, farStrength) : max(rawCoC, -nearStrength);
    }
    else
    {
        // Legacy: smoothstep ramps from focus plane
        float diff = depth - focusDistance;
        if (diff > 0.0)
            coc = smoothstep(farStart, farEnd, diff) * farStrength;
        else
            coc = -smoothstep(nearStart, nearEnd, -diff) * nearStrength;
    }

    return float4(coc, coc, coc, 1);
}
