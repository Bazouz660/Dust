// Generalized Kuwahara filter — 8-sector variant for oil-painting look.
// Splits neighborhood into 8 overlapping pie sectors, picks the mean of
// the sector with lowest variance. Produces flat color patches with
// sharp boundaries — the hallmark of painterly rendering.

Texture2D sceneTex : register(t0);
Texture2D<float> depthTex : register(t1);
SamplerState pointClamp : register(s0);

cbuffer KuwaharaParams : register(b0)
{
    float2 texelSize;
    int    radius;
    float  strength;
    float  sharpness;
    int    depthEnabled;
    float  depthStart;
    float  depthEnd;
    float  nearRadius;
    float  nearStrength;
    int    depthPreview;
    float  _pad;
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

    float effectiveRadius = clamp((float)radius, 0.0, 8.0);
    float effectiveStrength = saturate(strength);
    float distanceFade = 1.0;
    if (depthEnabled)
    {
        float depth = depthTex.SampleLevel(pointClamp, uv, 0);
        // Kenshi clears sky depth to zero. Treat it as the far endpoint.
        if (!(depth > 0.0) || !isfinite(depth)) depth = 1.0;
        float start = min(depthStart, depthEnd);
        float end = max(depthStart, depthEnd);
        float t = smoothstep(start, max(end, start + 1e-5), depth);
        distanceFade = t;
        effectiveRadius = lerp(clamp(nearRadius, 0.0, 8.0), effectiveRadius, t);
        effectiveStrength = lerp(saturate(nearStrength), effectiveStrength, t);
    }
    // Diagnose the distance ramp independently of filtering/endpoint strengths.
    // Magenta means depth modulation is disabled or its texture is unavailable.
    if (depthPreview)
        return depthEnabled ? float4(distanceFade.xxx, 1.0) : float4(1, 0, 1, 1);
    if (effectiveRadius <= 0.0 || effectiveStrength <= 0.0)
        return float4(original, 1.0);

    // Fade in the next ring of samples at fractional radii. This is continuous
    // through integer boundaries and preserves the original integer kernels.
    int r = (int)ceil(effectiveRadius);
    int innerR = (int)floor(effectiveRadius);
    float ringWeight = frac(effectiveRadius);
    float innerRSq = ((float)innerR + 0.5) * ((float)innerR + 0.5);
    float maxRSq = ((float)r + 0.5) * ((float)r + 0.5);

    float3 sSum[NUM_SECTORS];
    float3 sSumSq[NUM_SECTORS];
    float  sCount[NUM_SECTORS];

    [unroll]
    for (int s = 0; s < NUM_SECTORS; s++)
    {
        sSum[s] = float3(0, 0, 0);
        sSumSq[s] = float3(0, 0, 0);
        sCount[s] = 0.0;
    }

    // Single pass: fetch each texel once, accumulate into matching sectors
    [loop]
    for (int y = -r; y <= r; y++)
    {
        [loop]
        for (int x = -r; x <= r; x++)
        {
            float2 d = float2(x, y);
            float lenSq = dot(d, d);

            if (lenSq > maxRSq)
                continue;

            if (lenSq < 0.5)
            {
                [unroll]
                for (int s2 = 0; s2 < NUM_SECTORS; s2++)
                {
                    sSum[s2] += original;
                    sSumSq[s2] += original * original;
                    sCount[s2] += 1.0;
                }
                continue;
            }

            float weight = (abs(x) > innerR || abs(y) > innerR || lenSq > innerRSq) ? ringWeight : 1.0;
            if (weight <= 0.0) continue;
            float3 c = sceneTex.SampleLevel(pointClamp, uv + d * texelSize, 0).rgb;
            float2 nd = d * rsqrt(lenSq);

            [unroll]
            for (int s3 = 0; s3 < NUM_SECTORS; s3++)
            {
                if (dot(nd, sectorDir[s3]) >= 0.383)
                {
                    sSum[s3] += c * weight;
                    sSumSq[s3] += c * c * weight;
                    sCount[s3] += weight;
                }
            }
        }
    }

    float  minVar = 1e20;
    float3 filtered = original;

    [unroll]
    for (int i = 0; i < NUM_SECTORS; i++)
    {
        float inv = 1.0 / max(sCount[i], 1.0);
        float3 mean = sSum[i] * inv;
        float3 var = abs(sSumSq[i] * inv - mean * mean);
        float totalVar = var.r + var.g + var.b;
        if (totalVar < minVar)
        {
            minVar = totalVar;
            filtered = mean;
        }
    }

    return float4(lerp(original, filtered, effectiveStrength), 1.0);
}
