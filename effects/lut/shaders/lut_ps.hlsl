// HDR -> Tonemap -> LUT -> Dither in a single pass.
// Input:  HDR scene copy (R11G11B10_FLOAT)
// LUT:    R16G16B16A16_FLOAT (half-float precision, more than enough for 8-bit output)
// Output: LDR (B8G8R8A8_UNORM) — the ONLY quantization step in the entire chain.
// Vertex shader is provided by the framework via host->DrawFullscreenTriangle()

Texture2D hdrTex  : register(t0);
Texture2D lutTex  : register(t1);
SamplerState pointSamp  : register(s0);
SamplerState linearSamp : register(s1);

cbuffer LUTParams : register(b0) {
    float intensity;   // 0 = tonemapped only, 1 = fully graded
    float lutSize;     // number of slices (e.g. 32)
    float exposure;    // EV stops
    float padding;
};

// ACES filmic tone mapping (Narkowicz fit)
float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

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

// Triangular-PDF dither (+/-0.5 LSB in 8-bit)
float ditherNoise(float2 pos) {
    float n1 = frac(sin(dot(pos, float2(12.9898, 78.233)))  * 43758.5453);
    float n2 = frac(sin(dot(pos, float2(39.3461, 11.1350))) * 28471.3217);
    return (n1 + n2 - 1.0) / 255.0;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 hdr = hdrTex.SampleLevel(pointSamp, uv, 0).rgb;

    // Exposure adjustment in linear HDR space
    hdr *= exp2(exposure);

    // Tonemap: HDR -> [0,1] entirely in float precision (no 8-bit step)
    float3 ldr = ACESFilm(hdr);

    // LUT color grading (half-float LUT, sampled as float by GPU)
    float3 graded = SampleLUT(ldr, lutTex, linearSamp, lutSize);
    float3 result = lerp(ldr, graded, intensity);

    // Dither — the ONLY quantization in the entire pipeline
    result += ditherNoise(pos.xy);

    return float4(saturate(result), 1.0);
}
