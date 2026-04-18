// RTGI AO Composite Pass
// Applies ambient occlusion via multiply blend onto the HDR scene.
// When rendering at half-res, uses bilateral upscale guided by full-res depth.

Texture2D<float4> giTex    : register(t0); // Final denoised GI (A = AO)
Texture2D<float>  depthTex : register(t1);
SamplerState linearClamp   : register(s0);
SamplerState pointClamp    : register(s1);

cbuffer CompositeParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  giIntensity;
    float  saturation;
    float2 giTexSize;
};

float4 BilateralUpsample(float2 uv, float refDepth)
{
    float2 giInvSize = 1.0 / giTexSize;
    float2 giTexel = uv * giTexSize - 0.5;
    float2 f = frac(giTexel);
    float2 baseUV = (floor(giTexel) + 0.5) * giInvSize;

    float2 offsets[4] = {
        float2(0, 0), float2(giInvSize.x, 0),
        float2(0, giInvSize.y), float2(giInvSize.x, giInvSize.y)
    };

    float bilinWeights[4] = {
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y
    };

    float4 result = float4(0, 0, 0, 0);
    float  totalW = 0.0;

    // Fallback: nearest-depth sample. Critical for AO — multiply-blend turns
    // a degenerate ratio into black pixels at depth edges.
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

    if (totalW < 0.01)
        return bestGI;

    return result / totalW;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    float4 gi;
    if (giTexSize.x < viewportSize.x - 1.0)
        gi = BilateralUpsample(uv, depth);
    else
        gi = giTex.SampleLevel(linearClamp, uv, 0);

    float ao = gi.a;
    return float4(ao, ao, ao, 1.0);
}
