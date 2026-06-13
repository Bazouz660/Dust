// rt_common.hlsli — shared CB layout + helpers for the DustRT inline-RayQuery passes.
//
// Conventions (must match the host RtConstants struct and Kenshi's GBuffer —
// verified against data/materials/deferred/gbuffer.hlsl + deferred.hlsl):
//  - GBuffer depth (t0, R32F) is EUCLIDEAN VIEW DISTANCE / farClip in [0..1],
//    measured along the normalized pixel ray ("distance = depth * farClip;
//    viewPos = normalize(ray) * distance"). NOT view-space Z.
//  - GBuffer normals (t1) are WORLD-space, encoded *0.5+0.5.
//  - gCamForward points BEHIND the camera (OGRE RH): view +Z is -gCamForward.
//  - All world-space data (camera, TLAS instances) is in the GBuffer's absolute
//    OGRE world frame.

static const float RT_PI = 3.14159265358979;
static const float RT_TWO_PI = 6.28318530717959;

cbuffer RtCB : register(b0)
{
    float4 gCamRight;       // xyz = right,   w = tanHalfFovY
    float4 gCamUp;          // xyz = up,      w = aspect (w/h)
    float4 gCamForward;     // xyz = behind,  w = frame index
    float4 gCamPos;         // xyz = pos,     w = max ray distance
    float4 gPrevCamRight;   // xyz,           w = prev tanHalfFovY
    float4 gPrevCamUp;      // xyz,           w = prev aspect
    float4 gPrevCamForward; // xyz,           w = temporal alpha
    float4 gPrevCamPos;     // xyz,           w = history valid (0/1)
    float4 gSunDir;         // xyz = toward sun (normalized), w = tan(sun angular radius)
    float4 gSunColor;       // rgb = radiance*intensity,      w = shadow rays/pixel
    float4 gSkyHorizon;     // rgb,                            w = gi rays/pixel
    float4 gSkyZenith;      // rgb,                            w = bounce count
    float4 gGiParams;       // x = bounce albedo, y = normal bias, z = gi on, w = sun shadows on
    float4 gMisc;           // x = far clip (world units), y = atrous step, z = sigma depth, w = sigma normal
    float4 gRes;            // xy = resolution, zw = 1/resolution
}

SamplerState gPointClamp  : register(s0);
SamplerState gLinearClamp : register(s1);

// ---- camera reconstruction (identical math to the rtgi effect) -------------
float3 ViewPosFromDepth(float2 uv, float depth)
{
    float3 v;
    v.x = (uv.x * 2.0 - 1.0) * gCamUp.w * gCamRight.w * depth;
    v.y = (1.0 - uv.y * 2.0) * gCamRight.w * depth;
    v.z = depth;
    return v;
}

float3 ViewDirToWorld(float3 v)
{
    return v.x * gCamRight.xyz + v.y * gCamUp.xyz - v.z * gCamForward.xyz;
}

// Primary ray direction through a pixel (world space, normalized).
float3 PrimaryRayDir(float2 uv)
{
    return normalize(ViewDirToWorld(ViewPosFromDepth(uv, 1.0)));
}

// Kenshi depth validity: GBuffer clears to 0 (sky), far plane saturates to ~1.
bool Depth01Valid(float d01)
{
    return d01 > 0.00001 && d01 < 0.999;
}

float3 WorldPosFromDepth01(float2 uv, float d01)
{
    return gCamPos.xyz + PrimaryRayDir(uv) * (d01 * gMisc.x);
}

// Reprojection into the PREVIOUS camera. Returns uv and the expected
// normalized ray distance (same units as the depth buffer).
bool ReprojectPrev(float3 worldPos, out float2 uv, out float depth01)
{
    float3 rel = worldPos - gPrevCamPos.xyz;
    float3 v;
    v.x =  dot(rel, gPrevCamRight.xyz);
    v.y =  dot(rel, gPrevCamUp.xyz);
    v.z = -dot(rel, gPrevCamForward.xyz);
    depth01 = length(rel) / gMisc.x;
    if (v.z <= 0.01)
    {
        uv = float2(-1, -1);
        return false;
    }
    uv.x = (v.x / (v.z * gPrevCamUp.w * gPrevCamRight.w)) * 0.5 + 0.5;
    uv.y = 0.5 - (v.y / (v.z * gPrevCamRight.w)) * 0.5;
    return all(uv >= 0.0) && all(uv <= 1.0);
}

// ---- noise ------------------------------------------------------------------
float IGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Per-pixel, per-frame, per-sample decorrelated 2D xi in [0,1)
float2 Xi(uint2 px, float frame, uint sampleIdx)
{
    float n = IGN(float2(px) + frame * 5.588238);
    // R2 low-discrepancy rotation per sample index
    float2 r2 = frac(float2(0.754877666, 0.569840291) * (float)(sampleIdx + 1) + n);
    return r2;
}

float3 CosineHemisphereSample(float2 xi)
{
    float r = sqrt(xi.x);
    float phi = RT_TWO_PI * xi.y;
    float s, c;
    sincos(phi, s, c);
    return float3(r * c, r * s, sqrt(max(0.0, 1.0 - xi.x)));
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    if (n.z < -0.9999999) { t = float3(0, -1, 0); b = float3(-1, 0, 0); return; }
    float a = 1.0 / (1.0 + n.z);
    float d = -n.x * n.y * a;
    t = float3(1.0 - n.x * n.x * a, d, -n.x);
    b = float3(d, 1.0 - n.y * n.y * a, -n.y);
}

float3 AroundNormal(float3 local, float3 n)
{
    float3 t, b;
    BuildOrthonormalBasis(n, t, b);
    return local.x * t + local.y * b + local.z * n;
}

// Jittered direction toward the sun disk.
float3 SunDir(float2 xi)
{
    float r = sqrt(xi.x) * gSunDir.w;
    float phi = RT_TWO_PI * xi.y;
    float s, c;
    sincos(phi, s, c);
    float3 t, b;
    BuildOrthonormalBasis(gSunDir.xyz, t, b);
    return normalize(gSunDir.xyz + t * (r * c) + b * (r * s));
}

float3 SkyRadiance(float3 d)
{
    float t = sqrt(saturate(d.y));
    return lerp(gSkyHorizon.rgb, gSkyZenith.rgb, t);
}

float RtLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- scene fetch --------------------------------------------------------------
// Pool layout written by RtScene: per-instance {ibBase, vbBase} into the two pools.
struct RtInstInfo
{
    uint ibBase;    // first index of the range in gIndexPool
    uint vbBase;    // pool vertex the range's index 0 refers to
    uint flags;
    uint pad;
};

#ifdef RT_HAS_SCENE
RaytracingAccelerationStructure gTLAS : register(t2);
StructuredBuffer<uint>       gIndexPool : register(t3);
StructuredBuffer<float3>     gPosPool   : register(t4);
StructuredBuffer<RtInstInfo> gInstInfo  : register(t5);

// Geometric (facet) normal of a committed hit, world space, flipped toward -rayDir.
float3 FacetNormalWS(uint instId, uint primId, float3x4 objToWorld, float3 rayDir)
{
    RtInstInfo ii = gInstInfo[instId];
    uint i0 = gIndexPool[ii.ibBase + primId * 3 + 0];
    uint i1 = gIndexPool[ii.ibBase + primId * 3 + 1];
    uint i2 = gIndexPool[ii.ibBase + primId * 3 + 2];
    float3 p0 = gPosPool[ii.vbBase + i0];
    float3 p1 = gPosPool[ii.vbBase + i1];
    float3 p2 = gPosPool[ii.vbBase + i2];
    float3 n = cross(p1 - p0, p2 - p0);
    // rigid transform: rotate by the 3x3 part (rows of the world-from-object matrix)
    float3 nw = float3(dot(objToWorld[0].xyz, n),
                       dot(objToWorld[1].xyz, n),
                       dot(objToWorld[2].xyz, n));
    nw = normalize(nw);
    if (dot(nw, rayDir) > 0.0) nw = -nw;
    return nw;
}

// Binary visibility: true if something blocks o -> d within tmax.
bool Occluded(float3 o, float3 d, float tmax)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_FORCE_OPAQUE |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = o; r.Direction = d;
    r.TMin = 0.0; r.TMax = tmax;
    q.TraceRayInline(gTLAS, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#endif // RT_HAS_SCENE
