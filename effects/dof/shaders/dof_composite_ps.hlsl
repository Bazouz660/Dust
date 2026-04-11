// DOF composite — blends sharp original with blurred based on signed CoC.
// CoC > 0 = far blur, CoC < 0 = near blur, CoC == 0 = in focus.

Texture2D sceneTex       : register(t0);  // Original LDR scene (sharp)
Texture2D blurTex        : register(t1);  // Blurred scene (half-res, upsampled by linear filter)
Texture2D<float> cocTex  : register(t2);  // Signed circle of confusion map
SamplerState linearClamp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 sharp   = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 blurred = blurTex.SampleLevel(linearClamp, uv, 0).rgb;
    float  coc     = cocTex.SampleLevel(linearClamp, uv, 0).r;

    // Use absolute value for blend factor (both near and far blur the same way)
    float blendFactor = saturate(abs(coc));
    float3 result = lerp(sharp, blurred, blendFactor);
    return float4(result, 1.0);
}
