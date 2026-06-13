// Subsurface scattering — horizontal separable blur pass.
// Reads the lit HDR scene, blurs skin-tagged pixels along X with the skin
// diffusion profile; passes non-skin pixels through unchanged.

#include "sss_common.hlsli"

Texture2D    sceneTex    : register(t0);  // lit HDR scene copy
Texture2D    normalsTex  : register(t1);  // GBuffer normals; .a = skin tag
Texture2D    depthTex    : register(t2);  // euclidean depth / farClip
SamplerState linearClamp : register(s0);
SamplerState pointClamp  : register(s1);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return SSSBlurAxis(sceneTex, normalsTex, depthTex,
                       linearClamp, pointClamp, uv, float2(1.0, 0.0));
}
