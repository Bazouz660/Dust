// dpm_debug_ps.hlsl — fullscreen heatmap of triangle counts per DPM texel.
// Maps count to a viridis-like gradient: black -> blue -> green -> yellow -> red.
// 'maxDisplay' is the upper bound for normalization (typically the configured d).

cbuffer DebugCB : register(b0)
{
    float2 viewportSize;
    float  dpmSize;
    float  maxDisplay;
};

Texture2D<uint> g_CountMap : register(t0);
SamplerState    g_Samp     : register(s0);

float3 Heatmap(float t)
{
    t = saturate(t);
    // Quick viridis-ish ramp
    float3 c0 = float3(0.0, 0.0, 0.2);
    float3 c1 = float3(0.0, 0.3, 0.7);
    float3 c2 = float3(0.0, 0.7, 0.3);
    float3 c3 = float3(1.0, 0.9, 0.0);
    float3 c4 = float3(1.0, 0.2, 0.0);
    if (t < 0.25) return lerp(c0, c1, t / 0.25);
    if (t < 0.50) return lerp(c1, c2, (t - 0.25) / 0.25);
    if (t < 0.75) return lerp(c2, c3, (t - 0.50) / 0.25);
    return lerp(c3, c4, (t - 0.75) / 0.25);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // Show the DPM in the top-left quadrant at 1:1 mapping into the texture.
    // Outside this region, write nothing (preserve scene by returning 0 alpha — but
    // we don't have alpha blending on; instead just darken slightly).
    float2 inset = uv * 2.0; // top-left quarter sampled at full DPM range
    if (inset.x > 1.0 || inset.y > 1.0)
        return float4(0, 0, 0, 0); // outside: write black where we sample
                                   // (caller may want blend; for now we write black to that quad's outside area)

    int2 texel = int2(inset * dpmSize);
    uint count = g_CountMap.Load(int3(texel, 0));
    float t = (maxDisplay > 0.0) ? float(count) / maxDisplay : 0.0;
    float3 col = Heatmap(t);
    // Emphasize overflow (count >= maxDisplay) in pure white
    if (float(count) >= maxDisplay) col = float3(1, 1, 1);
    return float4(col, 1);
}
