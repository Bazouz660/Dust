#pragma once

// LUT apply pixel shader (post-tonemap, operates on LDR B8G8R8A8_UNORM)
// Vertex shader is provided by the framework via host->DrawFullscreenTriangle()
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
    color = saturate(color);

    float maxIndex = size - 1.0;
    float blueSlice = color.b * maxIndex;

    float slice0 = floor(blueSlice);
    float slice1 = min(slice0 + 1.0, maxIndex);
    float blueFrac = blueSlice - slice0;

    float2 uv;
    uv.y = (color.g * maxIndex + 0.5) / size;

    float u0 = (slice0 * size + color.r * maxIndex + 0.5) / (size * size);
    float u1 = (slice1 * size + color.r * maxIndex + 0.5) / (size * size);

    float3 c0 = lut.SampleLevel(samp, float2(u0, uv.y), 0).rgb;
    float3 c1 = lut.SampleLevel(samp, float2(u1, uv.y), 0).rgb;

    return lerp(c0, c1, blueFrac);
}

// Screen-space dither to break 8-bit banding (triangular PDF, +/-1 LSB)
float ditherNoise(float2 pos) {
    // Two independent hashes for triangular distribution
    float n1 = frac(sin(dot(pos, float2(12.9898, 78.233)))  * 43758.5453);
    float n2 = frac(sin(dot(pos, float2(39.3461, 11.1350))) * 28471.3217);
    // Triangular PDF [-1, +1] mapped to [-0.5/255, +0.5/255]
    return (n1 + n2 - 1.0) / 255.0;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 scene = sceneTex.SampleLevel(pointSamp, uv, 0).rgb;

    // Scene is already LDR [0,1] after the game's tonemapper — direct LUT lookup
    float3 graded = SampleLUT(scene, lutTex, linearSamp, lutSize);

    // Blend between original and graded
    float3 result = lerp(scene, graded, intensity);

    // Dither to break 8-bit banding in smooth gradients (sky, fog)
    float d = ditherNoise(pos.xy);
    result += float3(d, d, d);

    return float4(result, 1.0);
}
)";
