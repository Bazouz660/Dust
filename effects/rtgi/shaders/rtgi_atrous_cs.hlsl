// RTGI A-Trous Wavelet Denoise — Compute Shader
//
// References:
//   Dammertz et al. 2010, "Edge-Avoiding A-Trous Wavelet Transform for fast
//     Global Illumination Filtering" (EGSR 2010)
//   Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering" (HPG 2017)
//     - Section 4.3: variance-guided edge-stopping + variance propagation
//
// Key improvements over standard A-Trous:
//   1. Luminance edge-stopping modulated by per-pixel temporal variance
//   2. Variance propagated through iterations via w^2 rule (Schied 2017 Eq. 10)
//   3. Geometry-aware signal weight based on mean/variance comparison
//
// 8x8 tiles, one thread per output pixel. Dual UAV output (color + variance).

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

Texture2D<float4>   giTex       : register(t0);
Texture2D<float>    depthTex    : register(t1);
Texture2D<float4>   normalsTex  : register(t2);
Texture2D<float4>   varianceTex : register(t3);
RWTexture2D<float4> outTex      : register(u0);
RWTexture2D<float4> outVariance : register(u1);
SamplerState        pointClamp  : register(s0);

cbuffer DenoiseParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  stepSize;
    float  depthSigma;
    float  phiColor;
    float  fadeDistance;
    float  filterSmoothness;
    float  iteration;
};

// 3x3 B-spline kernel
static const float kernelWeights[3][3] = {
    { 0.44444, 0.66667, 0.44444 },
    { 0.66667, 1.00000, 0.66667 },
    { 0.44444, 0.66667, 0.44444 }
};

static const float pixelDists[3][3] = {
    { 1.41421, 1.00000, 1.41421 },
    { 1.00000, 0.00000, 1.00000 },
    { 1.41421, 1.00000, 1.41421 }
};

// Signal weight based on mean/variance comparison (Schied 2017)
float SignalWeight(float m1, float m2, float v1, float v2, float sharpness)
{
    float a = rsqrt(v1 + v1 + 1e-10);
    float bias = (m1 - m2) * a;
    return sqrt(v1) * a * exp(-0.5 * sharpness * bias * bias) * 1.414;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)viewportSize.x || tid.y >= (uint)viewportSize.y)
        return;

    int2 pix = int2(tid.xy);
    float2 uv = (float2(pix) + 0.5) * invViewportSize;

    float4 centerGI = giTex.Load(int3(pix, 0));
    float  centerDepth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (centerDepth <= 0.0001 || centerDepth > fadeDistance)
    {
        outTex[tid.xy] = centerGI;
        outVariance[tid.xy] = float4(0, 0, 0, 0);
        return;
    }

    float  centerLum = Luminance(centerGI.rgb);
    float  centerAO = centerGI.a;
    float3 centerNormal = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float  centerVar = varianceTex.Load(int3(pix, 0)).r;

    // Depth gradient for adaptive depth threshold
    float depthRight = depthTex.SampleLevel(pointClamp, uv + float2(invViewportSize.x, 0), 0);
    float depthDown  = depthTex.SampleLevel(pointClamp, uv + float2(0, invViewportSize.y), 0);
    float fwidthZ = max(abs(depthRight - centerDepth), abs(depthDown - centerDepth));

    // Variance-guided luminance threshold (Schied 2017 Eq. 9)
    float lumStdDev = sqrt(max(centerVar, 1e-10));
    float invPhiColor = 1.0 / max(phiColor * lumStdDev + 1e-4, 1e-6);

    // Sharpness from user setting
    float sharpness = saturate(1.0 - sqrt(max(filterSmoothness, 0.0)) * 0.98);
    sharpness *= sharpness;

    float3 sumColor = centerGI.rgb;
    float  wSumColor = 1.0;
    float  sumAO = centerAO;
    float  wSumAO = 1.0;
    float  sumVar = centerVar;
    float  wSumVar = 1.0;

    static const float AO_SIGMA_SQ2 = 2.0 * 4.0 * 4.0;
    int iStep = (int)stepSize;

    float4 minVal = centerGI;
    float4 maxVal = centerGI;

    float sampleVars[8];
    float sampleWeights[8];

    // First pass: compute geometry weights and cache variance
    [unroll]
    for (int idx = 0; idx < 8; idx++)
    {
        static const int2 offsets8[8] = {
            int2(-1,-1), int2(0,-1), int2(1,-1),
            int2(-1, 0),             int2(1, 0),
            int2(-1, 1), int2(0, 1), int2(1, 1)
        };

        int2 samplePix = pix + offsets8[idx] * iStep;

        if (any(samplePix < 0) || samplePix.x >= (int)viewportSize.x || samplePix.y >= (int)viewportSize.y)
        {
            sampleWeights[idx] = 0;
            sampleVars[idx] = 0;
            continue;
        }

        float2 sampleUV = (float2(samplePix) + 0.5) * invViewportSize;
        float sampleDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

        if (sampleDepth <= 0.0001)
        {
            sampleWeights[idx] = 0;
            sampleVars[idx] = 0;
            continue;
        }

        float3 sampleNormal = normalsTex.SampleLevel(pointClamp, sampleUV, 0).rgb * 2.0 - 1.0;

        // Map idx to 3x3 grid position (skipping center)
        int gx = (idx < 3) ? (idx) : (idx < 5) ? (idx == 3 ? 0 : 2) : (idx - 5);
        int gy = (idx < 3) ? 0 : (idx < 5) ? 1 : 2;
        float pixelDist = pixelDists[gy][gx];
        float kw = kernelWeights[gy][gx];

        // Depth weight
        float expectedChange = max(fwidthZ * pixelDist * stepSize, 1e-6);
        float wZ = abs(centerDepth - sampleDepth) / (expectedChange * depthSigma);
        float depthW = exp(-max(wZ, 0.0));

        // Normal weight
        float normalDot = max(0.0, dot(centerNormal, sampleNormal));
        float wN = saturate(exp((normalDot - 1.0) * 64.0));

        sampleWeights[idx] = lerp(0.001, 1.0, saturate(depthW * wN)) * kw;
        sampleVars[idx] = varianceTex.Load(int3(samplePix, 0)).r;
    }

    // Second pass: filter with signal-weight function
    [unroll]
    for (int j = 0; j < 8; j++)
    {
        if (sampleWeights[j] <= 0)
            continue;

        static const int2 offsets8b[8] = {
            int2(-1,-1), int2(0,-1), int2(1,-1),
            int2(-1, 0),             int2(1, 0),
            int2(-1, 1), int2(0, 1), int2(1, 1)
        };

        int2 samplePix = pix + offsets8b[j] * iStep;
        float4 sampleGI = giTex.Load(int3(samplePix, 0));

        // Map j to 3x3 grid position
        int gx = (j < 3) ? (j) : (j < 5) ? (j == 3 ? 0 : 2) : (j - 5);
        int gy = (j < 3) ? 0 : (j < 5) ? 1 : 2;
        float pixelDist = pixelDists[gy][gx];

        // Signal weight (Schied 2017): uses variance to judge if luminance difference is signal or noise
        float wSig = SignalWeight(centerLum, Luminance(sampleGI.rgb),
                                  1e-10 + centerVar, 1e-10 + sampleVars[j], sharpness);

        float wColor = sampleWeights[j] * wSig;
        sumColor += sampleGI.rgb * wColor;
        wSumColor += wColor;

        // Variance propagation (Schied 2017 Eq. 10): w^2 preserves variance correctly
        sumVar += sampleVars[j] * wColor * wColor;
        wSumVar += wColor;

        // AO: use geometry weights + spatial falloff
        float actualPixelDist = pixelDist * stepSize;
        float aoSpatialW = exp(-actualPixelDist * actualPixelDist / AO_SIGMA_SQ2);
        float wAO = sampleWeights[j] * aoSpatialW;
        sumAO += sampleGI.a * wAO;
        wSumAO += wAO;

        minVal = min(minVal, sampleGI);
        maxVal = max(maxVal, sampleGI);
    }

    // First iteration: clamp to neighborhood min/max (Karis 2014)
    if (iteration < 0.5)
    {
        centerGI = clamp(centerGI, minVal, maxVal);
    }

    float3 filteredColor = (sumColor) / max(wSumColor, 1e-6);
    float  filteredAO = sumAO / max(wSumAO, 1e-6);

    // Variance propagation: sum(w^2 * v) / sum(w)^2 (Schied 2017 Eq. 10)
    float propagatedVar = sumVar / (wSumVar * wSumVar + 1e-10);

    outTex[tid.xy] = float4(filteredColor, filteredAO);
    outVariance[tid.xy] = float4(propagatedVar, 0, 0, 0);
}
