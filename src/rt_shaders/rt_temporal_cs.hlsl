// rt_temporal_cs — temporal accumulation of the raw trace output.
//
// Reprojects the current pixel's world position into the previous camera and
// blends with history when the previous depth agrees (world-space anchored EMA;
// camera cuts / disocclusions fall back to the current sample).
//
// Inputs:  t0 depth, t1 normals; u0 giRaw (read), u1 history (read),
//          u2 accumOut (write), u3 prevDepth (read).

#include "rt_common.hlsli"

Texture2D<float>    gDepth     : register(t0);
Texture2D<float4>   gNormals   : register(t1);
RWTexture2D<float4> gRaw       : register(u0);
RWTexture2D<float4> gHistory   : register(u1);
RWTexture2D<float4> gAccumOut  : register(u2);
RWTexture2D<float>  gPrevDepth : register(u3);

float4 BilinearLoad(RWTexture2D<float4> tex, float2 uv)
{
    float2 p = uv * gRes.xy - 0.5;
    float2 f = frac(p);
    int2 i0 = int2(floor(p));
    int2 mx = int2(gRes.xy) - 1;
    int2 a = clamp(i0,              int2(0, 0), mx);
    int2 b = clamp(i0 + int2(1, 0), int2(0, 0), mx);
    int2 c = clamp(i0 + int2(0, 1), int2(0, 0), mx);
    int2 d = clamp(i0 + int2(1, 1), int2(0, 0), mx);
    return lerp(lerp(tex[a], tex[b], f.x), lerp(tex[c], tex[d], f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= (uint)gRes.x || px.y >= (uint)gRes.y)
        return;

    float4 cur = gRaw[px];
    float depth01 = gDepth[px];

    if (!Depth01Valid(depth01) || gPrevCamPos.w < 0.5)
    {
        gAccumOut[px] = cur;
        return;
    }

    float2 uv = (float2(px) + 0.5) * gRes.zw;
    float3 P = WorldPosFromDepth01(uv, depth01);

    float2 prevUV;
    float expected01;
    if (!ReprojectPrev(P, prevUV, expected01))
    {
        gAccumOut[px] = cur;
        return;
    }

    // depth agreement against last frame's depth buffer (normalized units)
    int2 ppx = clamp(int2(prevUV * gRes.xy), int2(0, 0), int2(gRes.xy) - 1);
    float prev01 = gPrevDepth[ppx];
    if (abs(prev01 - expected01) > 0.05 * max(expected01, 0.001))
    {
        gAccumOut[px] = cur;
        return;
    }

    float4 hist = BilinearLoad(gHistory, prevUV);
    float alpha = saturate(gPrevCamForward.w);
    gAccumOut[px] = lerp(hist, cur, alpha);
}
