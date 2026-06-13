// rt_debug_cs — primary-ray "clay" view of the TLAS, the ground-truth check
// that the captured acceleration structure matches the rasterized scene.
//
// Also writes a synthesized GBuffer (linear view-Z depth + encoded facet
// normals) so the standalone self-test can drive the trace/denoise passes
// without a rasterizer.
//
// Inputs:  t0 GBuffer depth (alignment check), t2 TLAS, t3 index pool,
//          t4 position pool, t5 instance info.
// Outputs: u0 clay color, u1 depth01, u2 normals (*0.5+0.5),
//          u3 alignment counters ([0] = GBuffer-valid pixels, [4] = pixels
//          where the traced depth matches the GBuffer within 3%).

#define RT_HAS_SCENE 1
#include "rt_common.hlsli"

Texture2D<float>    gDepthIn   : register(t0);
RWTexture2D<float4> gClay      : register(u0);
RWTexture2D<float>  gDepthOut  : register(u1);
RWTexture2D<float4> gNormalOut : register(u2);
RWByteAddressBuffer gAlign     : register(u3);

void CountAlignment(uint2 px, float synth01)
{
    float gb = gDepthIn[px];
    if (!Depth01Valid(gb))
        return;
    uint dummy;
    gAlign.InterlockedAdd(0, 1, dummy);
    if (synth01 > 0.0 && abs(synth01 - gb) < 0.03 * max(gb, 0.01))
        gAlign.InterlockedAdd(4, 1, dummy);
}

float3 InstanceTint(uint id)
{
    uint h = id * 2654435761u;
    return 0.65 + 0.35 * float3((h & 255) / 255.0, ((h >> 8) & 255) / 255.0, ((h >> 16) & 255) / 255.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= (uint)gRes.x || px.y >= (uint)gRes.y)
        return;

    float2 uv = (float2(px) + 0.5) * gRes.zw;
    float3 rd = PrimaryRayDir(uv);
    float3 ro = gCamPos.xyz;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc ray;
    ray.Origin = ro; ray.Direction = rd;
    ray.TMin = 0.0; ray.TMax = gCamPos.w;
    q.TraceRayInline(gTLAS, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        gClay[px] = float4(SkyRadiance(rd), 0.0);
        gDepthOut[px] = 0.0;   // matches the GBuffer's cleared-sky convention
        gNormalOut[px] = float4(0.5, 0.5, 0.5, 0);
        CountAlignment(px, 0.0);
        return;
    }

    float t = q.CommittedRayT();
    float3 n = FacetNormalWS(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                             q.CommittedObjectToWorld3x4(), rd);
    float3 hp = ro + rd * t;

    // simple clay shading: sun n.l + hemispherical fill, tinted per instance
    float ndl = saturate(dot(n, gSunDir.xyz));
    float3 col = InstanceTint(q.CommittedInstanceID()) * (0.25 + 0.75 * ndl);

    // shadow ray for the clay view (makes TLAS misalignment obvious)
    float bias = gGiParams.y * max(1.0, t * 0.01);
    if (ndl > 0.0 && Occluded(hp + n * bias, gSunDir.xyz, gCamPos.w))
        col *= 0.35;

    gClay[px] = float4(col, 1.0);

    // synthesized GBuffer in Kenshi's convention: ray distance / farClip
    float synth01 = t / gMisc.x;
    gDepthOut[px] = synth01;
    gNormalOut[px] = float4(n * 0.5 + 0.5, 0);
    CountAlignment(px, synth01);
}
