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

static const float kernel[3] = { 1.0, 2.0/3.0, 1.0/6.0 };

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 centerGI = giTex.SampleLevel(pointClamp, uv, 0);
    float centerDepth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (centerDepth <= 0.0001)
        return centerGI;

    float centerLum = Luminance(centerGI.rgb);
    float centerAO = centerGI.a;
    float3 centerNormal = normalize(normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0);

    // Depth gradient
    float depthRight = depthTex.SampleLevel(pointClamp, uv + float2(invViewportSize.x, 0), 0);
    float depthDown  = depthTex.SampleLevel(pointClamp, uv + float2(0, invViewportSize.y), 0);
    float fwidthZ = max(abs(depthRight - centerDepth), abs(depthDown - centerDepth));

    float  kw0 = kernel[0] * kernel[0];
    float3 sumColor = centerGI.rgb * kw0;
    float  wSumColor = kw0;
    float  sumAO = centerAO * kw0;
    float  wSumAO = kw0;

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

            float pixelDist = length(float2(dx, dy));
            float kw = kernel[abs(dx)] * kernel[abs(dy)];

            // ---- Depth weight ----
            float expectedChange = max(fwidthZ * pixelDist * stepSize, 1e-6);
            float wZ = abs(centerDepth - sampleDepth) / (expectedChange * depthSigma);
            float depthW = exp(-max(wZ, 0.0));

            // ---- Normal weight ----
            float normalDot = max(0.0, dot(centerNormal, sampleNormal));

            // ---- Color: depth + normal(pow16) + luminance ----
            float wN_color = pow(normalDot, 16.0);
            float lumDiff = abs(centerLum - Luminance(sampleGI.rgb));
            float wL = lumDiff / max(phiColor * 0.1, 1e-6);
            float wColor = depthW * wN_color * exp(-max(wL, 0.0)) * kw;
            sumColor += sampleGI.rgb * wColor;
            wSumColor += wColor;

            // ---- AO: depth + normal(pow8) + Gaussian spatial falloff ----
            float wN_ao = pow(normalDot, 8.0);
            float actualPixelDist = pixelDist * stepSize;
            float aoSpatialW = exp(-actualPixelDist * actualPixelDist / AO_SIGMA_SQ2);
            float wAO = depthW * wN_ao * kw * aoSpatialW;
            sumAO += sampleGI.a * wAO;
            wSumAO += wAO;
        }
    }

    float3 filteredColor = sumColor / max(wSumColor, 1e-6);
    float  filteredAO = sumAO / max(wSumAO, 1e-6);

    return float4(filteredColor, filteredAO);
}
