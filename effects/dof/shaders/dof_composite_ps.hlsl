// DOF composite — blends sharp original with blurred background based on CoC.

Texture2D sceneTex       : register(t0);  // Original LDR scene (sharp)
Texture2D blurTex        : register(t1);  // Blurred scene (half-res, upsampled by linear filter)
Texture2D<float> cocTex  : register(t2);  // Circle of confusion map
SamplerState linearClamp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 sharp   = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 blurred = blurTex.SampleLevel(linearClamp, uv, 0).rgb;
    float  coc     = cocTex.SampleLevel(linearClamp, uv, 0).r;

    float3 result = lerp(sharp, blurred, coc);
    return float4(result, 1.0);
}
