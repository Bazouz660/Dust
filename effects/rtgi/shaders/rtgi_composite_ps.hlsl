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
    float totalW = 0.0;

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        float2 sampleUV = baseUV + offsets[i];
        float4 gi = giTex.SampleLevel(pointClamp, sampleUV, 0);
        float sampleDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

        // Depth similarity weight — reject samples from different surfaces
        float depthDiff = abs(refDepth - sampleDepth) / max(refDepth, 0.001);
        float depthW = exp(-depthDiff * depthDiff / (2.0 * 0.01 * 0.01));

        float w = bilinWeights[i] * depthW;
        result += gi * w;
        totalW += w;
    }

    return result / max(totalW, 1e-6);
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
