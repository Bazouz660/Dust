// dpm_build_gs.hlsl — GS for DPM construction.
// Inputs the captured VS clip position, recovers world-space triangle vertices
// by applying the inverse light VP, and passes them to the PS for DPM storage.
// Slot 0: invLightVP + dpmSize. Slot 1: drawCallOffset (per-draw, updated by ReplayGeometryEx).

#pragma pack_matrix(row_major)

cbuffer DPMBuildCB : register(b0)
{
    float4x4 invLightVP;
    float    dpmSize;
    float    maxDepth;
    float2   _pad0;
};

cbuffer DrawOffsetCB : register(b1)
{
    uint  drawCallOffset;
    uint3 _pad1;
};

struct VSOut
{
    float4 posCS : SV_Position;
};

struct GSOut
{
    float4 posCS   : SV_Position;
    float3 v0WS    : TEXCOORD0;
    float3 v1WS    : TEXCOORD1;
    float3 v2WS    : TEXCOORD2;
    uint   primIdx : TEXCOORD3; // drawCallOffset + SV_PrimitiveID — globally unique
};

float3 ClipToWorld(float4 clip)
{
    // Row-vector convention: worldPos = clip * invLightVP
    float4 w = mul(clip, invLightVP);
    return w.xyz / w.w;
}

[maxvertexcount(3)]
void main(triangle VSOut IN[3], uint primID : SV_PrimitiveID,
          inout TriangleStream<GSOut> stream)
{
    float3 w0 = ClipToWorld(IN[0].posCS);
    float3 w1 = ClipToWorld(IN[1].posCS);
    float3 w2 = ClipToWorld(IN[2].posCS);
    uint   idx = drawCallOffset + primID;

    [unroll]
    for (int i = 0; i < 3; i++)
    {
        GSOut o;
        o.posCS   = IN[i].posCS;
        o.v0WS    = w0;
        o.v1WS    = w1;
        o.v2WS    = w2;
        o.primIdx = idx;
        stream.Append(o);
    }
    stream.RestartStrip();
}
