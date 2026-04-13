// RTGI AO Composite Pass
// Applies ambient occlusion via multiply blend onto the HDR scene.

Texture2D<float4> giTex  : register(t0); // Final denoised GI (A = AO)
SamplerState linearClamp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 gi = giTex.SampleLevel(linearClamp, uv, 0);
    float ao = gi.a; // 1 = unoccluded, 0 = fully occluded
    return float4(ao, ao, ao, 1.0);
}
