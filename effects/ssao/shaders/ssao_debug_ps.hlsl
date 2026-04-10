// AO debug — no-blend pass. Overwrites left third with raw AO visualization.
// Scales output high so auto-exposure can't wash out the contrast.

Texture2D<float> aoTex     : register(t0);
SamplerState     samLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    if (uv.x >= 0.333)
        discard;

    float ao = aoTex.Sample(samLinear, uv);
    // Scale AO into a low range so auto-exposure amplifies it back up.
    // Preserves the white=unoccluded, dark=occluded look regardless of exposure.
    float v = ao * 0.01;
    return float4(v, v, v, 1.0);
}
