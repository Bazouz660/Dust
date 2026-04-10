// Debug visualization for DOF — shows the CoC map.
// Black = in focus, white = maximum blur.

Texture2D<float> cocTex  : register(t0);
SamplerState pointClamp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float coc = cocTex.Sample(pointClamp, uv);
    return float4(coc, coc, coc, 1.0);
}
