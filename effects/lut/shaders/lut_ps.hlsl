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
    int   tonemapper;  // see Tonemap() below — matches gLUTSettingsArray enum order
    float whitePoint;  // Reinhard Extended only — luminance that maps to pure white
};

// ======================== Tonemapping operators ========================
// All operators consume linear HDR input and return [0,1] LDR output.
// Keep order in sync with gTonemapperLabels in DustLUT.cpp.

// 0 — Linear clamp (reference / debug)
float3 TM_Linear(float3 x) { return saturate(x); }

// 1 — Reinhard (basic)
float3 TM_Reinhard(float3 x) { return x / (1.0 + x); }

// 2 — Reinhard Extended with whitepoint (preserves bright highlights)
float3 TM_ReinhardExt(float3 x, float W) {
    return (x * (1.0 + x / (W * W))) / (1.0 + x);
}

// 3 — ACES Narkowicz (fast approximate fit — original)
float3 TM_ACESNarkowicz(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}

// 4 — ACES Hill (full RRT+ODT fit via input/output matrices — more accurate)
float3 TM_ACESHill(float3 x) {
    static const float3x3 ACESInputMat = float3x3(
        0.59719, 0.35458, 0.04823,
        0.07600, 0.90834, 0.01566,
        0.02840, 0.13383, 0.83777);
    static const float3x3 ACESOutputMat = float3x3(
         1.60475, -0.53108, -0.07367,
        -0.10208,  1.10813, -0.00605,
        -0.00327, -0.07276,  1.07602);
    float3 v = mul(ACESInputMat, x);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    v = a / b;
    return mul(ACESOutputMat, v);
}

// 5 — Uncharted 2 / Hable filmic (warmer shoulder)
float3 TM_Uncharted2Partial(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float3 TM_Uncharted2(float3 x) {
    const float exposureBias = 2.0;
    float3 curr = TM_Uncharted2Partial(x * exposureBias);
    float3 whiteScale = 1.0 / TM_Uncharted2Partial(float3(11.2, 11.2, 11.2));
    return curr * whiteScale;
}

// 6 — AgX (Blender-style minimal fit — sigmoid in log space + input/output matrices)
float3 TM_AgXContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return  15.5    * x4 * x2
          - 40.14   * x4 * x
          + 31.96   * x4
          - 6.868   * x2 * x
          + 0.4298  * x2
          + 0.1191  * x
          - 0.00232;
}
float3 TM_AgX(float3 x) {
    static const float3x3 agxIn = float3x3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    static const float3x3 agxOut = float3x3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);
    const float minEV = -12.47393;
    const float maxEV =   4.026069;

    x = max(x, 1e-10);
    float3 v = mul(agxIn, x);
    v = clamp(log2(v), minEV, maxEV);
    v = (v - minEV) / (maxEV - minEV);
    v = TM_AgXContrastApprox(v);
    // Inverse AgX matrix + 2.2 EOTF → sRGB linear (the 2D LUT then re-grades in that space)
    v = mul(agxOut, v);
    return pow(max(v, 0.0), 2.2);
}

// 7 — Khronos PBR Neutral (glTF standard, hue-preserving with highlight desat)
float3 TM_PBRNeutral(float3 x) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float minChan = min(x.r, min(x.g, x.b));
    float offset = (minChan < 0.08) ? (minChan - 6.25 * minChan * minChan) : 0.04;
    x -= offset;
    float peak = max(x.r, max(x.g, x.b));
    if (peak < startCompression) return x;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    x *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(x, float3(newPeak, newPeak, newPeak), g);
}

float3 Tonemap(float3 x, int mode) {
    if (mode == 0) return TM_Linear(x);
    if (mode == 1) return saturate(TM_Reinhard(x));
    if (mode == 2) return saturate(TM_ReinhardExt(x, whitePoint));
    if (mode == 3) return saturate(TM_ACESNarkowicz(x));
    if (mode == 4) return saturate(TM_ACESHill(x));
    if (mode == 5) return saturate(TM_Uncharted2(x));
    if (mode == 6) return saturate(TM_AgX(x));
    if (mode == 7) return saturate(TM_PBRNeutral(x));
    return saturate(TM_ACESNarkowicz(x));
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
    float3 ldr = Tonemap(hdr, tonemapper);

    // LUT color grading (half-float LUT, sampled as float by GPU)
    float3 graded = SampleLUT(ldr, lutTex, linearSamp, lutSize);
    float3 result = lerp(ldr, graded, intensity);

    // Dither — the ONLY quantization in the entire pipeline
    result += ditherNoise(pos.xy);

    return float4(saturate(result), 1.0);
}
