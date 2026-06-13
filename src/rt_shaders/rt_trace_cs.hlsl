// rt_trace_cs — per-pixel sun shadow rays + path-traced indirect diffuse (GI).
//
// Inputs:  t0 depth (linear view-Z), t1 world normals (*0.5+0.5),
//          t2 TLAS, t3 index pool, t4 position pool, t5 instance info.
// Output:  u0 = float4(gi radiance estimate (albedo-demodulated), sun visibility).
//
// GI estimator: cosine-sampled hemisphere; with pdf = cos/pi the outgoing
// indirect radiance is albedo * mean(Li), so this pass stores mean(Li) and the
// composite multiplies by the GBuffer albedo. Hit surfaces are shaded as grey
// Lambertian (gGiParams.x) lit by the sun (next-event ray) and sky on miss.

#define RT_HAS_SCENE 1
#include "rt_common.hlsli"

Texture2D<float>  gDepth   : register(t0);
Texture2D<float4> gNormals : register(t1);
RWTexture2D<float4> gOut   : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= (uint)gRes.x || px.y >= (uint)gRes.y)
        return;

    float2 uv = (float2(px) + 0.5) * gRes.zw;
    float depth01 = gDepth[px];

    // sky / invalid depth
    if (!Depth01Valid(depth01))
    {
        gOut[px] = float4(0, 0, 0, 1);
        return;
    }

    float dist = depth01 * gMisc.x;
    float3 n = normalize(gNormals[px].rgb * 2.0 - 1.0);
    float3 P = WorldPosFromDepth01(uv, depth01);
    float bias = gGiParams.y * max(1.0, dist * 0.01);
    float3 O = P + n * bias;
    float frame = gCamForward.w;
    float maxDist = gCamPos.w;

    // ---- sun visibility -------------------------------------------------
    float sunVis = 1.0;
    if (gGiParams.w > 0.5)
    {
        int rays = max(1, (int)gSunColor.w);
        float hits = 0.0;
        [loop]
        for (int i = 0; i < rays; ++i)
        {
            float3 sd = SunDir(Xi(px, frame, (uint)i));
            // surfaces facing away from the sun are shadowed regardless
            if (dot(n, sd) <= 0.0) { hits += 1.0; continue; }
            if (Occluded(O, sd, maxDist)) hits += 1.0;
        }
        sunVis = 1.0 - hits / (float)rays;
    }

    // ---- path-traced indirect diffuse ------------------------------------
    float3 gi = 0.0;
    if (gGiParams.z > 0.5)
    {
        int rays = max(1, (int)gSkyHorizon.w);
        int bounces = max(1, (int)gSkyZenith.w);

        [loop]
        for (int r = 0; r < rays; ++r)
        {
            float3 throughput = 1.0;
            float3 ro = O;
            float3 rn = n;

            [loop]
            for (int b = 0; b < bounces; ++b)
            {
                float3 rd = AroundNormal(CosineHemisphereSample(Xi(px, frame, (uint)(r * 8 + b * 2 + 13))), rn);

                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
                RayDesc ray;
                ray.Origin = ro; ray.Direction = rd;
                ray.TMin = 0.0; ray.TMax = maxDist;
                q.TraceRayInline(gTLAS, RAY_FLAG_NONE, 0xFF, ray);
                q.Proceed();

                if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                {
                    gi += throughput * SkyRadiance(rd);
                    break;
                }

                float3 hn = FacetNormalWS(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                          q.CommittedObjectToWorld3x4(), rd);
                float3 hp = ro + rd * q.CommittedRayT() + hn * bias;

                throughput *= gGiParams.x;   // grey hit albedo

                // next-event estimation: sun at the hit point
                float3 sd = SunDir(Xi(px, frame, (uint)(r * 8 + b * 2 + 14)));
                float ndl = dot(hn, sd);
                if (ndl > 0.0 && !Occluded(hp, sd, maxDist))
                    gi += throughput * gSunColor.rgb * ndl / RT_PI;

                ro = hp;
                rn = hn;
            }
        }
        gi /= (float)rays;
    }

    gOut[px] = float4(gi, sunVis);
}
