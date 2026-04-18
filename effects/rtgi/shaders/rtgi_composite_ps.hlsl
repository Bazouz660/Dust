// RTGI Composite Pass — applies GI intensity scaling at the final stage.
// Drawn with additive blend onto the HDR scene.
// When rendering at half-res, uses bilateral upscale guided by full-res depth.

Texture2D<float4> giTex    : register(t0);
Texture2D<float>  depthTex : register(t1);
SamplerState linearClamp   : register(s0);
SamplerState pointClamp    : register(s1);

cbuffer CompositeParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  giIntensity;
    float  saturation;
    float2 giTexSize;       // Half-res or full-res GI texture dimensions
};

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 BilateralUpsample(float2 uv, float refDepth)
{
    // Half-pixel offset in GI texture space
    float2 giInvSize = 1.0 / giTexSize;
    float2 giTexel = uv * giTexSize - 0.5;
    float2 f = frac(giTexel);
    float2 baseUV = (floor(giTexel) + 0.5) * giInvSize;

    // 4 nearest GI texels
    float2 offsets[4] = {
        float2(0, 0), float2(giInvSize.x, 0),
        float2(0, giInvSize.y), float2(giInvSize.x, giInvSize.y)
    };

    // Bilinear weights
    float bilinWeights[4] = {
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y
    };

    float4 result = float4(0, 0, 0, 0);
    float  totalW = 0.0;

    // Fallback: track the single sample whose depth is closest to refDepth.
    // When the full-res pixel sits on a surface that none of the 4 nearest
    // half-res neighbours match (common at foreground edges against the sky
    // or distant background), every depthW collapses to ~0. Without this
    // fallback, totalW → 0, the divide degenerates, and we get black pixels.
    float4 bestGI = float4(0, 0, 0, 1);
    float  bestDepthDiff = 1e20;

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        float2 sampleUV = baseUV + offsets[i];
        float4 gi = giTex.SampleLevel(pointClamp, sampleUV, 0);
        float sampleDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

        float depthDiff = abs(refDepth - sampleDepth) / max(refDepth, 0.001);
        float depthW = exp(-depthDiff * depthDiff / (2.0 * 0.01 * 0.01));

        float w = bilinWeights[i] * depthW;
        result += gi * w;
        totalW += w;

        if (depthDiff < bestDepthDiff)
        {
            bestDepthDiff = depthDiff;
            bestGI = gi;
        }
    }

    // If every neighbour was rejected (bilateral weights collapsed), prefer the
    // single least-wrong sample over a degenerate ratio.
    if (totalW < 0.01)
        return bestGI;

    return result / totalW;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    // Use bilateral upscale when GI is lower res than output
    float4 gi;
    if (giTexSize.x < viewportSize.x - 1.0)
        gi = BilateralUpsample(uv, depth);
    else
        gi = giTex.SampleLevel(linearClamp, uv, 0);

    float3 indirect = gi.rgb * giIntensity;

    // Saturation control
    float lum = Luminance(indirect);
    indirect = lerp(float3(lum, lum, lum), indirect, saturation);

    return float4(indirect, 1.0);
}
