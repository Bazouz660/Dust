// RTGI Variance Pass
// Computes per-pixel luminance standard deviation in a 3x3 window from the
// temporal accumulation output, stored in .r of a single-channel RT.
// The a-trous denoiser reads this pre-computed value once per iteration
// instead of recomputing it locally (which was ~9 taps per iteration × 4).

Texture2D<float4> giTex      : register(t0);
SamplerState      pointClamp : register(s0);

cbuffer VarianceParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
};

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerLum = Luminance(giTex.SampleLevel(pointClamp, uv, 0).rgb);
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
    return float4(lumStdDev, 0, 0, 0);
}
