// RTGI A-Trous Wavelet Denoise — Compute Shader
//
// Same math as the earlier PS version (3×3 kernel, normal + depth + luminance
// edge-stopping, luminance stddev read from the variance pass). Switching to
// compute removes RTV/OM/blend/viewport setup from the CPU hot path and lets
// the HW scheduler pack threads without the PS 2×2 quad constraint, so both
// CPU submission and GPU occupancy improve.
//
// 8×8 tiles, one thread per output pixel. No LDS caching — the 3×3 kernel
// with variable step size (1/2/4/8) doesn't reuse enough samples across
// neighbouring threads to beat the texture cache at large step sizes.

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

Texture2D<float4>   giTex       : register(t0);
Texture2D<float>    depthTex    : register(t1);
Texture2D<float4>   normalsTex  : register(t2);
Texture2D<float>    varianceTex : register(t3);
RWTexture2D<float4> outTex      : register(u0);
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
    float  _pad0;
    float  _pad1;
};

// 3x3 B-spline kernel: 1D {2/3, 1, 2/3}, tensor product for 2D
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

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // Bounds check — dispatch is ceil(width/8), so the last tile may overshoot.
    if (tid.x >= (uint)viewportSize.x || tid.y >= (uint)viewportSize.y)
        return;

    float2 uv = (float2(tid.xy) + 0.5) * invViewportSize;

    float4 centerGI = giTex.SampleLevel(pointClamp, uv, 0);
    float  centerDepth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (centerDepth <= 0.0001 || centerDepth > fadeDistance)
    {
        outTex[tid.xy] = centerGI;
        return;
    }

    float  centerLum = Luminance(centerGI.rgb);
    float  centerAO = centerGI.a;
    float3 centerNormal = normalize(normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0);

    float lumStdDev = varianceTex.SampleLevel(pointClamp, uv, 0);

    // Depth gradient
    float depthRight = depthTex.SampleLevel(pointClamp, uv + float2(invViewportSize.x, 0), 0);
    float depthDown  = depthTex.SampleLevel(pointClamp, uv + float2(0, invViewportSize.y), 0);
    float fwidthZ = max(abs(depthRight - centerDepth), abs(depthDown - centerDepth));

    float invPhiColor = 1.0 / max(phiColor * lumStdDev + 1e-4, 1e-6);

    float3 sumColor = centerGI.rgb;
    float  wSumColor = 1.0;
    float  sumAO = centerAO;
    float  wSumAO = 1.0;

    static const float AO_SIGMA_SQ2 = 2.0 * 4.0 * 4.0;

    [unroll]
    for (int dy = -1; dy <= 1; dy++)
    {
        [unroll]
        for (int dx = -1; dx <= 1; dx++)
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

            float pixelDist = pixelDists[dy + 1][dx + 1];
            float kw = kernelWeights[dy + 1][dx + 1];

            // ---- Depth weight ----
            float expectedChange = max(fwidthZ * pixelDist * stepSize, 1e-6);
            float wZ = abs(centerDepth - sampleDepth) / (expectedChange * depthSigma);
            float depthW = exp(-max(wZ, 0.0));

            // ---- Normal weight ----
            float normalDot = max(0.0, dot(centerNormal, sampleNormal));

            // ---- Color: depth + normal(pow16) + luminance ----
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
            float wAO = depthW * n8 * kw * aoSpatialW;
            sumAO += sampleGI.a * wAO;
            wSumAO += wAO;
        }
    }

    float3 filteredColor = sumColor / max(wSumColor, 1e-6);
    float  filteredAO = sumAO / max(wSumAO, 1e-6);

    outTex[tid.xy] = float4(filteredColor, filteredAO);
}
