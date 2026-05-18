// Reinterleave pass: maps raw AO back from deinterleaved to screen-coherent
// layout. At each output pixel (screen space), read from the deinterleaved
// source position. Skipped tiles (per shading rate) keep their previous-frame
// value.

#include "ao_pipeline.hlsli"

Texture2D<float4> RawOccTex : register(t0);

struct VSOUT
{
    float4 vpos : SV_Position;
    float2 uv   : TEXCOORD0;
};

float4 main(VSOUT i) : SV_Target
{
    uint2 dispatchthreadid = (uint2)floor(i.vpos.xy);
    uint  tile = (uint)DeinterleaveTileCount;
    uint2 read_pos = deinterleave_pos(dispatchthreadid, tile, (uint2)BufferScreenSize);
    uint2 tilesize = ((uint2)BufferScreenSize + tile - 1) / tile;
    uint2 tile_idx = dispatchthreadid / tilesize;

    if (shading_rate(tile_idx))
        discard;

    return RawOccTex.Load(int3(read_pos, 0));
}
