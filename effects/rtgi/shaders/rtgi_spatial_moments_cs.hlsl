// RTGI Spatial Moments — Compute Shader (half render resolution)
//
// Computes spatially-blurred luminance statistics for both the current raw GI
// and the previous accumulated GI. The temporal pass uses these to derive a
// variance-ratio alpha that adapts blending to scene dynamics.
//
// Reference: Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering"
//            HPG 2017 — Section 4.2 (temporal integration with variance guidance)
//
// 8x8 groups, 16x16 groupshared tile (4-pixel border for 7x7 Gaussian kernel).

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

Texture2D<float4>   currentGI   : register(t0);
Texture2D<float4>   historyGI   : register(t1);
RWTexture2D<float4> outTex      : register(u0);
SamplerState        linearClamp : register(s0);

cbuffer SpatialMomentsParams : register(b0)
{
    float2 halfResSize;
    float2 invHalfResSize;
    float2 fullResSize;
    float2 invFullResSize;
};

struct MomentData
{
    float currLum;
    float histLum;
};

groupshared MomentData tile[16 * 16];

[numthreads(8, 8, 1)]
void main(uint3 groupId : SV_GroupID,
          uint3 groupThreadId : SV_GroupThreadID,
          uint3 dispatchId : SV_DispatchThreadID,
          uint  threadIdx : SV_GroupIndex)
{
    [unroll]
    for (int y = 0; y < 2; y++)
    [unroll]
    for (int x = 0; x < 2; x++)
    {
        int2 halfResPix = int2(groupId.xy) * 8 + int2(groupThreadId.xy) + int2(x, y) * 8 - 4;
        float2 uv = (float2(halfResPix * 2) + 1.0) * invFullResSize;

        float3 cCol = currentGI.SampleLevel(linearClamp, uv, 0).rgb;
        float3 hCol = historyGI.SampleLevel(linearClamp, uv, 0).rgb;

        int2 ldsPos = int2(groupThreadId.xy) + int2(x, y) * 8;
        MomentData md;
        md.currLum = Luminance(cCol);
        md.histLum = Luminance(hCol);
        tile[ldsPos.y * 16 + ldsPos.x] = md;
    }

    GroupMemoryBarrierWithGroupSync();

    float4 result = float4(0, 0, 0, 0);
    float wsum = 0;

    [unroll]
    for (int ky = -3; ky <= 3; ky++)
    [unroll]
    for (int kx = -3; kx <= 3; kx++)
    {
        int2 ldsPos = int2(groupThreadId.xy) + 4 + int2(kx, ky);
        float w = exp(-float(kx * kx + ky * ky) * 0.2222);
        MomentData md = tile[ldsPos.y * 16 + ldsPos.x];
        result.x += md.currLum * w;
        result.y += md.currLum * md.currLum * w;
        result.z += md.histLum * w;
        result.w += md.histLum * md.histLum * w;
        wsum += w;
    }

    result /= wsum;
    result.y = sqrt(max(0, result.y - result.x * result.x));
    result.w = sqrt(max(0, result.w - result.z * result.z));

    if (dispatchId.x < (uint)halfResSize.x && dispatchId.y < (uint)halfResSize.y)
        outTex[dispatchId.xy] = result;
}
