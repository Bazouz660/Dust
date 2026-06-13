// Subsurface scattering — vertical separable blur pass.
// Reads the horizontally-blurred HDR, blurs skin-tagged pixels along Y; passes
// non-skin pixels through unchanged.

#include "sss_common.hlsli"

Texture2D    sceneTex    : register(t0);  // horizontally-blurred HDR
Texture2D    normalsTex  : register(t1);  // GBuffer normals; .a = skin tag
Texture2D    depthTex    : register(t2);  // euclidean depth / farClip
SamplerState linearClamp : register(s0);
SamplerState pointClamp  : register(s1);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return SSSBlurAxis(sceneTex, normalsTex, depthTex,
                       linearClamp, pointClamp, uv, float2(0.0, 1.0));
}
