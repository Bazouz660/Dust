// Composite Screen Space Shadows onto the HDR scene.
// The shadow mask is applied via multiply blending (set on the CPU side).
// This shader simply outputs the shadow mask value.

Texture2D<float> sssTex      : register(t0);
SamplerState     linearClamp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float shadow = sssTex.Sample(linearClamp, uv);
    return float4(shadow, shadow, shadow, 1.0);
}
