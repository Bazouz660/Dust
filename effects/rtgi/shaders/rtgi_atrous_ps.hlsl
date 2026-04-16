// RTGI A-Trous Wavelet Denoise — Normal + Depth Edge-Stopping
//
// GI color: full a-trous with depth + normal + luminance weights
// AO: ALL iterations contribute, but with a Gaussian spatial falloff that
//     naturally limits the effective blur radius (~12px). This gives many more
//     effective samples than a hard 2-iteration cutoff, while still preserving
//     AO gradients. Normals protect crevice boundaries.

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

Texture2D<float4> giTex       : register(t0);
Texture2D<float>  depthTex    : register(t1);
Texture2D<float4> normalsTex  : register(t2);
SamplerState      pointClamp  : register(s0);

cbuffer DenoiseParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  stepSize;
    float  depthSigma;
    float  phiColor;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

// Precomputed 5x5 kernel weights: kernel[abs(dx)] * kernel[abs(dy)]
// kernel = { 1.0, 2/3, 1/6 }
static const float kernelWeights[5][5] = {
    { 0.02778, 0.11111, 0.16667, 0.11111, 0.02778 },
    { 0.11111, 0.44444, 0.66667, 0.44444, 0.11111 },
    { 0.16667, 0.66667, 1.00000, 0.66667, 0.16667 },
    { 0.11111, 0.44444, 0.66667, 0.44444, 0.11111 },
    { 0.02778, 0.11111, 0.16667, 0.11111, 0.02778 }
};

// Precomputed distances: length(float2(dx, dy)) for dx,dy in [-2..2]
static const float pixelDists[5][5] = {
    { 2.82843, 2.23607, 2.00000, 2.23607, 2.82843 },
    { 2.23607, 1.41421, 1.00000, 1.41421, 2.23607 },
    { 2.00000, 1.00000, 0.00000, 1.00000, 2.00000 },
    { 2.23607, 1.41421, 1.00000, 1.41421, 2.23607 },
    { 2.82843, 2.23607, 2.00000, 2.23607, 2.82843 }
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 centerGI = giTex.SampleLevel(pointClamp, uv, 0);
    float centerDepth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (centerDepth <= 0.0001)
        return centerGI;

    float centerLum = Luminance(centerGI.rgb);
    float centerAO = centerGI.a;
    float3 centerNormal = normalize(normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0);

    // Local 3x3 luminance variance — phiColor is scaled by stddev so weights
    // relax in noisy regions and tighten at real edges (variance-guided SVGF).
    float lumSum = centerLum;
    float lumSumSq = centerLum * centerLum;
    [unroll]
    for (int vy = -1; vy <= 1; vy++)
    {
        [unroll]
        for (int vx = -1; vx <= 1; vx++)
        {
            if (vx == 0 && vy == 0) continue;
            float3 vc = giTex.SampleLevel(pointClamp, uv + float2(vx, vy) * invViewportSize, 0).rgb;
            float vl = Luminance(vc);
            lumSum += vl;
            lumSumSq += vl * vl;
        }
    }
    float lumMean = lumSum * (1.0 / 9.0);
    float lumStdDev = sqrt(max(lumSumSq * (1.0 / 9.0) - lumMean * lumMean, 0.0));

    // Depth gradient
    float depthRight = depthTex.SampleLevel(pointClamp, uv + float2(invViewportSize.x, 0), 0);
    float depthDown  = depthTex.SampleLevel(pointClamp, uv + float2(0, invViewportSize.y), 0);
    float fwidthZ = max(abs(depthRight - centerDepth), abs(depthDown - centerDepth));

    // phiColor now behaves like "how many sigmas of tolerance" — multiplied by local stddev.
    // Epsilon prevents collapse in perfectly flat regions (lumStdDev == 0).
    float invPhiColor = 1.0 / max(phiColor * lumStdDev + 1e-4, 1e-6);

    float  kw0 = 1.0; // kernelWeights[2][2]
    float3 sumColor = centerGI.rgb;
    float  wSumColor = 1.0;
    float  sumAO = centerAO;
    float  wSumAO = 1.0;

    // AO spatial falloff
    static const float AO_SIGMA_SQ2 = 2.0 * 4.0 * 4.0;

    [loop]
    for (int dy = -2; dy <= 2; dy++)
    {
        [loop]
        for (int dx = -2; dx <= 2; dx++)
        {
            if (dx == 0 && dy == 0)
                continue;

            float2 sampleUV = uv + float2(dx, dy) * invViewportSize * stepSize;

            if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
                continue;

            float sampleDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

            if (sampleDepth <= 0.0001)
                continue;

            float4 sampleGI = giTex.SampleLevel(pointClamp, sampleUV, 0);
            float3 sampleNormal = normalize(normalsTex.SampleLevel(pointClamp, sampleUV, 0).rgb * 2.0 - 1.0);

            float pixelDist = pixelDists[dy + 2][dx + 2];
            float kw = kernelWeights[dy + 2][dx + 2];

            // ---- Depth weight ----
            float expectedChange = max(fwidthZ * pixelDist * stepSize, 1e-6);
            float wZ = abs(centerDepth - sampleDepth) / (expectedChange * depthSigma);
            float depthW = exp(-max(wZ, 0.0));

            // ---- Normal weight ----
            float normalDot = max(0.0, dot(centerNormal, sampleNormal));

            // ---- Color: depth + normal(pow16) + luminance ----
            // pow(x, 16) = ((x^2)^2)^2)^2 — 4 multiplies instead of pow()
            float n2 = normalDot * normalDot;
            float n4 = n2 * n2;
            float n8 = n4 * n4;
            float wN_color = n8 * n8; // = normalDot^16

            float lumDiff = abs(centerLum - Luminance(sampleGI.rgb));
            float wL = lumDiff * invPhiColor;
            float wColor = depthW * wN_color * exp(-max(wL, 0.0)) * kw;
            sumColor += sampleGI.rgb * wColor;
            wSumColor += wColor;

            // ---- AO: depth + normal(pow8) + Gaussian spatial falloff ----
            float actualPixelDist = pixelDist * stepSize;
            float aoSpatialW = exp(-actualPixelDist * actualPixelDist / AO_SIGMA_SQ2);
            float wAO = depthW * n8 * kw * aoSpatialW; // n8 = normalDot^8
            sumAO += sampleGI.a * wAO;
            wSumAO += wAO;
        }
    }

    float3 filteredColor = sumColor / max(wSumColor, 1e-6);
    float  filteredAO = sumAO / max(wSumAO, 1e-6);

    return float4(filteredColor, filteredAO);
}
