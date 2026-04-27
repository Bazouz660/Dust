// dpm_build_ps.hlsl — DPM construction pixel shader.
// For each rasterized fragment, atomically increments the per-texel triangle count,
// stores the world-space triangle vertices in the prim buffer (all pixels of the same
// triangle write the same data to the same slot — idempotent), and records the prim index
// in the per-texel slot list.

#pragma pack_matrix(row_major)

cbuffer DPMBuildCB : register(b0)
{
    float4x4 invLightVP;  // unused in PS but must match GS CB layout
    float    dpmSize;
    float    maxDepth;    // = gConfig.maxPrimsPerTexel
    float2   _pad0;
};

cbuffer DrawOffsetCB : register(b1)
{
    uint  drawCallOffset;
    uint3 _pad1;
};

struct DPMPrim { float v[9]; }; // float3 v0, float3 v1, float3 v2 (36 bytes, no padding)

RWTexture2D<uint>          g_CountMap   : register(u0); // triangle count per DPM texel
RWStructuredBuffer<uint>   g_IndicesMap : register(u1); // prim indices, [texelFlat * d + slot]
RWStructuredBuffer<DPMPrim> g_PrimBuf   : register(u2); // world-space triangles, [primIdx]

// Safety upper bound for slot arithmetic. maxDepth from CB controls actual writes.
// If the user changes maxPrimsPerTexel beyond 64, a rebuild is needed.
static const uint DEPTH_LIMIT = 64;

struct GSOut
{
    float4 posCS   : SV_Position;
    float3 v0WS    : TEXCOORD0;
    float3 v1WS    : TEXCOORD1;
    float3 v2WS    : TEXCOORD2;
    uint   primIdx : TEXCOORD3;
};

void main(GSOut IN)
{
    uint2 texel = uint2(IN.posCS.xy);
    uint  uDpmSize = (uint)dpmSize;

    // Write triangle vertices to the prim buffer.
    // All fragments of the same triangle write the same data to the same slot.
    uint idx = IN.primIdx;
    g_PrimBuf[idx].v[0] = IN.v0WS.x; g_PrimBuf[idx].v[1] = IN.v0WS.y; g_PrimBuf[idx].v[2] = IN.v0WS.z;
    g_PrimBuf[idx].v[3] = IN.v1WS.x; g_PrimBuf[idx].v[4] = IN.v1WS.y; g_PrimBuf[idx].v[5] = IN.v1WS.z;
    g_PrimBuf[idx].v[6] = IN.v2WS.x; g_PrimBuf[idx].v[7] = IN.v2WS.y; g_PrimBuf[idx].v[8] = IN.v2WS.z;

    // Atomically claim a slot in this texel's index list.
    uint slot;
    InterlockedAdd(g_CountMap[texel], 1, slot);

    uint uMaxDepth = (uint)maxDepth;
    if (slot < uMaxDepth && slot < DEPTH_LIMIT)
    {
        uint flatIdx = (texel.y * uDpmSize + texel.x) * uMaxDepth + slot;
        g_IndicesMap[flatIdx] = idx;
    }
}
