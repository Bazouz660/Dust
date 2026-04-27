// dpm_raytrace_cs.hlsl — Ray tracing pass for Deep Primitive Map shadows.
// One thread per screen pixel: reconstructs world position, projects to DPM coordinates,
// iterates over stored triangles and runs Möller-Trumbore ray-triangle intersection.
// Output: R8_UNORM shadow mask (1 = lit, 0 = shadow).

#pragma pack_matrix(row_major)

cbuffer RayTraceCB : register(b0)
{
    float4x4 inverseView;   // world from view (row-major)
    float4x4 lightVP;       // world to light clip (row-major)
    float3   sunDir;        // world-space unit vector toward sun
    float    tanHalfFov;
    float2   viewportSize;
    float    dpmSize;
    float    maxDepth;
    uint     primBufSize;
    float3   _pad;
};

Texture2D<float>  depthTex   : register(t0); // Kenshi's R32_FLOAT custom depth (linear view-z)
Texture2D<float4> normalsTex : register(t1); // Kenshi GBuffer normals (XYZ in [0,1])
Texture2D<uint>   countMap   : register(t2); // DPM triangle count per texel
StructuredBuffer<uint>    indicesMap : register(t3); // DPM prim indices
StructuredBuffer<float>   primBuf    : register(t4); // flat float array, 9 floats per prim

RWTexture2D<float> shadowMask : register(u0);

SamplerState pointSamp : register(s0);

static const float RAY_TMIN = 0.01f; // avoid self-intersection

// Reconstruct view-space position from Kenshi's linear depth buffer.
// Kenshi stores view-space Z (positive = into scene, OGRE left-hand screen space).
float3 ReconstructViewPos(float2 uv, float depth)
{
    float aspect = viewportSize.x / viewportSize.y;
    float3 pos;
    pos.x =  (uv.x * 2.0 - 1.0) * aspect * tanHalfFov * depth;
    pos.y = -(uv.y * 2.0 - 1.0) * tanHalfFov * depth; // flip Y: screen top = +Y world-up
    pos.z =  depth;
    return pos;
}

// Möller-Trumbore ray-triangle intersection.
// Returns true and sets t if the ray hits the triangle at distance t > RAY_TMIN.
bool RayHitsTriangle(float3 orig, float3 dir,
                     float3 v0, float3 v1, float3 v2,
                     out float t)
{
    t = 0;
    float3 e1 = v1 - v0;
    float3 e2 = v2 - v0;
    float3 pv = cross(dir, e2);
    float  det = dot(e1, pv);
    if (abs(det) < 1e-8) return false;
    float  invDet = 1.0 / det;
    float3 tv = orig - v0;
    float  u = dot(tv, pv) * invDet;
    if (u < 0.0 || u > 1.0) return false;
    float3 qv = cross(tv, e1);
    float  v = dot(dir, qv) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;
    t = dot(e2, qv) * invDet;
    return t > RAY_TMIN;
}

void LoadPrim(uint idx, out float3 v0, out float3 v1, out float3 v2)
{
    uint base = idx * 9;
    v0 = float3(primBuf[base+0], primBuf[base+1], primBuf[base+2]);
    v1 = float3(primBuf[base+3], primBuf[base+4], primBuf[base+5]);
    v2 = float3(primBuf[base+6], primBuf[base+7], primBuf[base+8]);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 px = DTid.xy;
    if (px.x >= (uint)viewportSize.x || px.y >= (uint)viewportSize.y)
    {
        return;
    }

    float depth = depthTex.Load(int3(px, 0));

    // Sky: no geometry, always lit
    if (depth <= 0.0 || depth >= 1e9)
    {
        shadowMask[px] = 1.0;
        return;
    }

    // Decode Kenshi GBuffer normals (stored as XYZ in [0,1])
    float3 nEnc = normalsTex.Load(int3(px, 0)).xyz;
    float3 normal = nEnc * 2.0 - 1.0;

    // NdotL backface cull: self-shadowed surfaces (facing away from sun)
    float NdotL = dot(normalize(normal), sunDir);
    if (NdotL <= 0.0)
    {
        shadowMask[px] = 0.0; // self-shadowed
        return;
    }

    // Reconstruct view-space then world-space position
    float2 uv = (float2(px) + 0.5) / viewportSize;
    float3 viewPos = ReconstructViewPos(uv, depth);
    // Row-vector: worldPos = float4(viewPos, 1) * inverseView
    float4 worldPos4 = mul(float4(viewPos, 1.0), inverseView);
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // Project to DPM / light-clip space (row-vector convention)
    float4 lightClip = mul(float4(worldPos, 1.0), lightVP);
    float3 ndcLight = lightClip.xyz / lightClip.w;

    // Check coverage: [-1,1] x/y, [0,1] z
    if (ndcLight.x < -1.0 || ndcLight.x > 1.0 ||
        ndcLight.y < -1.0 || ndcLight.y > 1.0 ||
        ndcLight.z <  0.0 || ndcLight.z > 1.0)
    {
        // Outside sun frustum — fall back to lit (game's shadow handles far regions)
        shadowMask[px] = 1.0;
        return;
    }

    // DPM texel coordinates
    float2 dpmUV = ndcLight.xy * 0.5 + 0.5;
    uint2  dpmTexel = uint2(dpmUV * dpmSize);
    dpmTexel = clamp(dpmTexel, uint2(0,0), uint2((uint)dpmSize-1, (uint)dpmSize-1));

    uint count = min(countMap.Load(int3(dpmTexel, 0)), (uint)maxDepth);

    if (count == 0)
    {
        // No geometry at this DPM texel — lit
        shadowMask[px] = 1.0;
        return;
    }

    float3 rayDir = sunDir; // sun is directional; ray direction is constant

    bool  shadowed = false;
    uint uMaxDepth = (uint)maxDepth;
    uint flatBase = (dpmTexel.y * (uint)dpmSize + dpmTexel.x) * uMaxDepth;

    for (uint i = 0; i < count && !shadowed; i++)
    {
        uint primIdx = indicesMap[flatBase + i];
        if (primIdx >= primBufSize) continue;

        float3 v0, v1, v2;
        LoadPrim(primIdx, v0, v1, v2);

        float t;
        if (RayHitsTriangle(worldPos, rayDir, v0, v1, v2, t))
            shadowed = true;
    }

    shadowMask[px] = shadowed ? 0.0 : 1.0;
}
