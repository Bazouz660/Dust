// Spatial filter pass 1 (optional): local linear regression. Discards if
// FilterSize < 2. Fits AO = slope * depth + intercept over a 4x4 Gather4
// window and evaluates at center depth.

#include "ao_pipeline.hlsli"

Texture2D<float4> OccTex    : register(t0);
SamplerState      PointSamp : register(s0);

struct VSOUT
{
    float4 vpos : SV_Position;
    float2 uv   : TEXCOORD0;
};

float2 filter(float2 uv, int iter)
{
    float g = OccTex.SampleLevel(PointSamp, uv, 0).y;
    bool blurry = g < 0;
    float flip = iter ? -1.0 : 1.0;

    float4 ao, depth, mv = 0;

    ao    = OccTex.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2(-0.5, -0.5));
    depth = abs(OccTex.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2(-0.5, -0.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = OccTex.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2( 1.5, -0.5));
    depth = abs(OccTex.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2( 1.5, -0.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = OccTex.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2(-0.5,  1.5));
    depth = abs(OccTex.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2(-0.5,  1.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = OccTex.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2( 1.5,  1.5));
    depth = abs(OccTex.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2( 1.5,  1.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    mv /= 16.0;

    float b = (mv.w - mv.x * mv.z) / max(mv.y - mv.x * mv.x, exp2(blurry ? -12 : -30));
    float a = mv.z - b * mv.x;
    return float2(saturate(b * abs(g) + a), g);
}

float4 main(VSOUT i) : SV_Target
{
    [branch]
    if (FilterSize < 2)
        discard;

    float2 f = filter(i.uv, 0);
    return float4(f.x, f.y, 0.0, 0.0);
}
