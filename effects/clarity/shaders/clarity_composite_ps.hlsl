// Clarity composite — enhances local contrast by adding back the detail layer.
// detail = original - blurred (high-frequency content)
// output = original + detail * strength, masked to midtones to avoid clipping.

Texture2D sceneTex       : register(t0);  // Original LDR scene
Texture2D blurTex        : register(t1);  // Blurred (low-frequency) scene
SamplerState linearClamp : register(s0);

cbuffer ClarityParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  strength;
    float  midtoneProtect;
    float  blurRadius;
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 original = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 blurred  = blurTex.SampleLevel(linearClamp, uv, 0).rgb;

    // High-frequency detail layer
    float3 detail = original - blurred;

    // Midtone mask: protects shadows and highlights from clipping.
    // Peak response at mid-luminance, fades toward black and white.
    float luma = dot(original, float3(0.299, 0.587, 0.114));
    float midtoneMask = smoothstep(0.0, 0.25, luma) * smoothstep(1.0, 0.75, luma);
    float mask = lerp(1.0, midtoneMask, midtoneProtect);

    float3 result = original + detail * strength * mask;
    return float4(saturate(result), 1.0);
}
