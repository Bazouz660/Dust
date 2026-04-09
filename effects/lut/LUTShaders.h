#pragma once

// Fullscreen triangle vertex shader (no vertex buffer needed)
static const char* LUT_VS = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// LUT apply pixel shader
// Expects a 1024x32 strip texture (32 slices of 32x32 for a 32^3 LUT)
static const char* LUT_PS = R"(
Texture2D sceneTex : register(t0);
Texture2D lutTex   : register(t1);
SamplerState pointSamp  : register(s0);
SamplerState linearSamp : register(s1);

cbuffer LUTParams : register(b0) {
    float intensity;   // 0 = original, 1 = fully graded
    float lutSize;     // number of slices (e.g. 32)
    float2 padding;
};

float3 SampleLUT(float3 color, Texture2D lut, SamplerState samp, float size) {
    // Clamp to valid LUT range
    color = saturate(color);

    float maxIndex = size - 1.0;
    float blueSlice = color.b * maxIndex;

    float slice0 = floor(blueSlice);
    float slice1 = min(slice0 + 1.0, maxIndex);
    float blueFrac = blueSlice - slice0;

    // Each slice is 'size' pixels wide in the strip
    float2 uv;
    uv.y = (color.g * maxIndex + 0.5) / size;

    float u0 = (slice0 * size + color.r * maxIndex + 0.5) / (size * size);
    float u1 = (slice1 * size + color.r * maxIndex + 0.5) / (size * size);

    float3 c0 = lut.SampleLevel(samp, float2(u0, uv.y), 0).rgb;
    float3 c1 = lut.SampleLevel(samp, float2(u1, uv.y), 0).rgb;

    return lerp(c0, c1, blueFrac);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 scene = sceneTex.SampleLevel(pointSamp, uv, 0).rgb;

    // Tonemap to [0,1] for LUT lookup, then reverse
    // Simple Reinhard for the LUT lookup
    float3 mapped = scene / (scene + 1.0);

    float3 graded = SampleLUT(mapped, lutTex, linearSamp, lutSize);

    // Reverse tonemap back to HDR
    graded = graded / max(1.0 - graded, 0.001);

    // Blend between original and graded
    float3 result = lerp(scene, graded, intensity);

    return float4(result, 1.0);
}
)";
