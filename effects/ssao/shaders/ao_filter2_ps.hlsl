// Spatial filter pass 2: final filter. Matches original pipeline structure:
//   FilterSize 0: passthrough (no filtering)
//   FilterSize 1: single regression pass on reinterleaved AO
//   FilterSize 2: regression on filter1 output
// Skips scene-color composite (Kenshi's deferred shader does that via slot 8).

#include "ao_pipeline.hlsli"

Texture2D<float4> FilterTex0 : register(t0); // reinterleaved AO
Texture2D<float4> FilterTex1 : register(t1); // intermediate filtered AO
SamplerState      PointSamp  : register(s0);

struct VSOUT
{
    float4 vpos : SV_Position;
    float2 uv   : TEXCOORD0;
};

float2 filter_tex0(float2 uv, int iter)
{
    float g = FilterTex0.SampleLevel(PointSamp, uv, 0).y;
    bool blurry = g < 0;
    float flip = iter ? -1.0 : 1.0;

    float4 ao, depth, mv = 0;

    ao    = FilterTex0.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2(-0.5, -0.5));
    depth = abs(FilterTex0.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2(-0.5, -0.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = FilterTex0.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2( 1.5, -0.5));
    depth = abs(FilterTex0.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2( 1.5, -0.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = FilterTex0.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2(-0.5,  1.5));
    depth = abs(FilterTex0.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2(-0.5,  1.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = FilterTex0.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2( 1.5,  1.5));
    depth = abs(FilterTex0.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2( 1.5,  1.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    mv /= 16.0;

    float b = (mv.w - mv.x * mv.z) / max(mv.y - mv.x * mv.x, exp2(blurry ? -12 : -30));
    float a = mv.z - b * mv.x;
    return float2(saturate(b * abs(g) + a), g);
}

float2 filter_tex1(float2 uv, int iter)
{
    float g = FilterTex1.SampleLevel(PointSamp, uv, 0).y;
    bool blurry = g < 0;
    float flip = iter ? -1.0 : 1.0;

    float4 ao, depth, mv = 0;

    ao    = FilterTex1.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2(-0.5, -0.5));
    depth = abs(FilterTex1.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2(-0.5, -0.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = FilterTex1.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2( 1.5, -0.5));
    depth = abs(FilterTex1.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2( 1.5, -0.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = FilterTex1.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2(-0.5,  1.5));
    depth = abs(FilterTex1.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2(-0.5,  1.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = FilterTex1.GatherRed  (PointSamp, uv + flip * BufferPixelSize * float2( 1.5,  1.5));
    depth = abs(FilterTex1.GatherGreen(PointSamp, uv + flip * BufferPixelSize * float2( 1.5,  1.5)));
    mv += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    mv /= 16.0;

    float b = (mv.w - mv.x * mv.z) / max(mv.y - mv.x * mv.x, exp2(blurry ? -12 : -30));
    float a = mv.z - b * mv.x;
    return float2(saturate(b * abs(g) + a), g);
}

float4 main(VSOUT i) : SV_Target
{
    float2 result;
    if (FilterSize == 2)
        result = filter_tex1(i.uv, 1);
    else if (FilterSize == 1)
        result = filter_tex0(i.uv, 1);
    else
    {
        float4 src = FilterTex0.SampleLevel(PointSamp, i.uv, 0);
        result = float2(src.x, src.y);
    }

    // Allow small brightening for noise smoothing, but cap large
    // increases that cause bright halos at contact shadow edges.
    if (FilterSize > 0)
    {
        float rawAO = FilterTex0.SampleLevel(PointSamp, i.uv, 0).x;
        result.x = min(result.x, rawAO + 0.05);
    }

    return float4(result.x, result.y, 0.0, 0.0);
}
