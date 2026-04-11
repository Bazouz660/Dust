// Debug visualization for DOF — shows the signed CoC map.
// Red = far field blur, Blue = near field blur, Black = in focus.

Texture2D<float> cocTex  : register(t0);
SamplerState pointClamp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float coc = cocTex.Sample(pointClamp, uv);
    float farBlur = saturate(coc);        // positive = far
    float nearBlur = saturate(-coc);      // negative = near
    return float4(farBlur, 0.0, nearBlur, 1.0);
}
