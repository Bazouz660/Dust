// rt_atrous_cs — edge-stopping à-trous filter over the accumulated GI + sun
// visibility (rgb = GI radiance, a = sun visibility share the same edge weights).
//
// Run N times with gMisc.y = step size (1, 2, 4, ...). Inputs: t0 depth,
// t1 normals; u0 src (read), u1 dst (write).

#include "rt_common.hlsli"

Texture2D<float>    gDepth   : register(t0);
Texture2D<float4>   gNormals : register(t1);
RWTexture2D<float4> gSrc     : register(u0);
RWTexture2D<float4> gDst     : register(u1);

static const float kKernel[3] = { 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= (uint)gRes.x || px.y >= (uint)gRes.y)
        return;

    float depth = gDepth[px];
    if (!Depth01Valid(depth))
    {
        gDst[px] = gSrc[px];
        return;
    }

    float3 n = normalize(gNormals[px].rgb * 2.0 - 1.0);
    int step = max(1, (int)gMisc.y);
    float sigmaZ = gMisc.z * max(depth, 0.001);   // relative tolerance (depth is 0..1)
    float sigmaN = gMisc.w;

    float4 sum = 0.0;
    float wsum = 0.0;
    int2 mx = int2(gRes.xy) - 1;

    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 q = clamp(int2(px) + int2(dx, dy) * step, int2(0, 0), mx);
            float zq = gDepth[q];
            float3 nq = normalize(gNormals[q].rgb * 2.0 - 1.0);

            float wz = exp(-abs(zq - depth) / max(sigmaZ, 1e-4));
            float wn = pow(saturate(dot(n, nq)), sigmaN);
            float wk = kKernel[abs(dx)] * kKernel[abs(dy)];
            float w = wz * wn * wk;

            sum += gSrc[q] * w;
            wsum += w;
        }
    }

    gDst[px] = wsum > 1e-6 ? sum / wsum : gSrc[px];
}
