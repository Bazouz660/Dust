// Log-luminance extraction for auto-exposure.
// Output: log2(luminance) in R channel.
// After GenerateMips, the 1x1 mip contains the average log-luminance.

Texture2D hdrTex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 color = hdrTex.SampleLevel(samp, uv, 0).rgb;
    float lum = dot(color, float3(0.2126, 0.7152, 0.0722));
    return float4(log2(max(lum, 0.0001)), 0, 0, 1);
}
