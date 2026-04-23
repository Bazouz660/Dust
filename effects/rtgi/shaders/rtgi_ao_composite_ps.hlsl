// RTGI AO Composite Pass
// Applies ambient occlusion via multiply blend onto the HDR scene.
// When rendering below native res, uses joint bilateral upscale guided
// by full-res depth and normals (3x3 kernel).

Texture2D<float4> giTex      : register(t0);
Texture2D<float>  depthTex   : register(t1);
Texture2D<float4> normalsTex : register(t2);
SamplerState linearClamp     : register(s0);
SamplerState pointClamp      : register(s1);

cbuffer CompositeParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  giIntensity;
    float  saturation;
    float2 giTexSize;
};

float4 BilateralUpsample(float2 uv, float refDepth, float3 refNormal)
{
    float2 giInvSize = 1.0 / giTexSize;
    float2 giTexel = uv * giTexSize - 0.5;
    float2 f = frac(giTexel);
    float2 baseUV = (floor(giTexel) + 0.5) * giInvSize;

    float scaleRatio = viewportSize.x / giTexSize.x;
    float depthSigma = 0.01 * max(scaleRatio, 1.0);

    float4 result = float4(0, 0, 0, 0);
    float  totalW = 0.0;

    // Fallback: nearest-depth sample. Critical for AO — multiply-blend turns
    // a degenerate ratio into black pixels at depth edges.
    float4 bestGI = float4(0, 0, 0, 1);
    float  bestDepthDiff = 1e20;

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float2 sampleUV = baseUV + float2(x, y) * giInvSize;
            float4 gi = giTex.SampleLevel(pointClamp, sampleUV, 0);
            float sampleDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

            float2 d = float2(x, y) - f;
            float spatialW = exp(-dot(d, d) * 0.5);

            float depthDiff = abs(refDepth - sampleDepth) / max(refDepth, 0.001);
            float depthW = exp(-depthDiff * depthDiff / (2.0 * depthSigma * depthSigma));

            float3 sn = normalsTex.SampleLevel(pointClamp, sampleUV, 0).xyz * 2.0 - 1.0;
            float normalW = saturate(dot(refNormal, sn));
            normalW *= normalW;

            float w = spatialW * depthW * normalW;
            result += gi * w;
            totalW += w;

            if (depthDiff < bestDepthDiff)
            {
                bestDepthDiff = depthDiff;
                bestGI = gi;
            }
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
    {
        float3 normal = normalize(normalsTex.SampleLevel(pointClamp, uv, 0).xyz * 2.0 - 1.0);
        gi = BilateralUpsample(uv, depth, normal);
    }
    else
    {
        gi = giTex.SampleLevel(linearClamp, uv, 0);
    }

    float ao = gi.a;
    return float4(ao, ao, ao, 1.0);
}
