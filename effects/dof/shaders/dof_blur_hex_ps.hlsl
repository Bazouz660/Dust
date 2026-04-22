// Hexagonal blur - 6-blade aperture bokeh shape (replaces separable Gaussian)
// Single-pass gather with grid sampling and hex distance masking

Texture2D sceneTex       : register(t0);
SamplerState linearClamp : register(s0);

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
    int gridSize = clamp((int)ceil(blurRadius), 1, 6);
    float invGrid = 1.0 / float(gridSize);

    float3 total = float3(0, 0, 0);
    float totalWeight = 0.0;

    [loop]
    for (int y = -gridSize; y <= gridSize; y++)
    {
        [loop]
        for (int x = -gridSize; x <= gridSize; x++)
        {
            float2 p = float2(x, y) * invGrid;

            // Flat-top hexagonal distance
            float2 q = abs(p);
            float hexDist = max(q.x * 0.866025 + q.y * 0.5, q.y);

            if (hexDist <= 1.0)
            {
                float2 offset = p * blurRadius * texelSize;
                total += sceneTex.SampleLevel(linearClamp, uv + offset, 0).rgb;
                totalWeight += 1.0;
            }
        }
    }

    if (totalWeight < 1.0) totalWeight = 1.0;
    return float4(total / totalWeight, 1.0);
}
