// Generalized Kuwahara filter — 8-sector variant for oil-painting look.
// Splits neighborhood into 8 overlapping pie sectors, picks the mean of
// the sector with lowest variance. Produces flat color patches with
// sharp boundaries — the hallmark of painterly rendering.

Texture2D sceneTex : register(t0);
SamplerState pointClamp : register(s0);

cbuffer KuwaharaParams : register(b0)
{
    float2 texelSize;
    int    radius;
    float  strength;
    float  sharpness;
    float3 _pad;
};

static const int NUM_SECTORS = 8;

// Sector direction vectors (8 evenly spaced around the circle)
static const float2 sectorDir[NUM_SECTORS] = {
    float2( 1.0,  0.0),
    float2( 0.707,  0.707),
    float2( 0.0,  1.0),
    float2(-0.707,  0.707),
    float2(-1.0,  0.0),
    float2(-0.707, -0.707),
    float2( 0.0, -1.0),
    float2( 0.707, -0.707)
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 original = sceneTex.SampleLevel(pointClamp, uv, 0).rgb;

    int r = radius;

    float3 sectorMean[NUM_SECTORS];
    float  sectorVar[NUM_SECTORS];

    // For each sector, accumulate samples that fall within a 90-degree cone
    [unroll]
    for (int s = 0; s < NUM_SECTORS; s++)
    {
        float3 sum = 0.0;
        float3 sumSq = 0.0;
        int count = 0;

        [loop]
        for (int y = -r; y <= r; y++)
        {
            [loop]
            for (int x = -r; x <= r; x++)
            {
                if (x == 0 && y == 0)
                {
                    // Center pixel belongs to all sectors
                    float3 c = original;
                    sum += c;
                    sumSq += c * c;
                    count++;
                    continue;
                }

                float2 d = float2(x, y);
                float len = length(d);

                // Skip samples outside the radius circle
                if (len > (float)r + 0.5)
                    continue;

                // Check if this sample falls in this sector's cone (±45 degrees)
                float2 nd = d / len;
                float dp = dot(nd, sectorDir[s]);
                if (dp < 0.383) // cos(67.5°) — 135° cone overlap between neighbors
                    continue;

                float2 offset = float2(x, y) * texelSize;
                float3 c = sceneTex.SampleLevel(pointClamp, uv + offset, 0).rgb;
                sum += c;
                sumSq += c * c;
                count++;
            }
        }

        float inv = 1.0 / max((float)count, 1.0);
        sectorMean[s] = sum * inv;
        float3 var = abs(sumSq * inv - sectorMean[s] * sectorMean[s]);
        sectorVar[s] = var.r + var.g + var.b;
    }

    // Hard selection: pick the sector with the lowest variance
    float  minVar = sectorVar[0];
    float3 filtered = sectorMean[0];

    [unroll]
    for (int i = 1; i < NUM_SECTORS; i++)
    {
        if (sectorVar[i] < minVar)
        {
            minVar = sectorVar[i];
            filtered = sectorMean[i];
        }
    }

    float3 result = lerp(original, filtered, strength);
    return float4(result, 1.0);
}
