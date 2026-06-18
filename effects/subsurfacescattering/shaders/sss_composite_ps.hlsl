// Subsurface scattering — composite pass.
// On skin-tagged pixels: blend original lit HDR with the blurred skin color by
// `strength`. On everything else: pass the original HDR through unchanged.

#include "sss_common.hlsli"

Texture2D    sceneTex    : register(t0);  // original lit HDR scene copy
Texture2D    blurTex     : register(t1);  // H+V blurred skin HDR
Texture2D    normalsTex  : register(t2);  // GBuffer normals; .a = skin tag
SamplerState linearClamp : register(s0);
SamplerState pointClamp  : register(s1);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 original = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float  tag      = normalsTex.SampleLevel(pointClamp, uv, 0).a;

    if (!IsSkinTag(tag))
        return float4(original, 1.0);

    float3 blurred = blurTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 result  = lerp(original, blurred, saturate(strength));
    return float4(result, 1.0);
}

// Standalone debug overlay: tints skin-tagged pixels magenta so the tag
// coverage is visible. Selected via the host's centralized debug-view registry
// and drawn onto the final LDR target. t0 = final LDR scene copy,
// t2 = GBuffer normals (.a = skin tag).
float4 debug_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 original = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float  tag      = normalsTex.SampleLevel(pointClamp, uv, 0).a;

    if (!IsSkinTag(tag))
        return float4(original, 1.0);

    return float4(original * 0.25 + float3(0.6, 0.0, 0.6), 1.0);
}
