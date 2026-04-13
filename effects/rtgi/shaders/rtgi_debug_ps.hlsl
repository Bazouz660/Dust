// RTGI Debug Visualization
// Displays GI components for debugging and tuning.

Texture2D<float4> giTex : register(t0);
SamplerState pointClamp : register(s0);

cbuffer DebugParams : register(b0)
{
    float debugMode; // 1=GI, 2=AO, 3=raw indirect
    float _pad0;
    float _pad1;
    float _pad2;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 gi = giTex.SampleLevel(pointClamp, uv, 0);

    int mode = (int)debugMode;

    if (mode == 1)
    {
        // GI indirect light only
        return float4(gi.rgb, 1.0);
    }
    else if (mode == 2)
    {
        // AO only (white = unoccluded, dark = occluded)
        return float4(gi.aaa, 1.0);
    }
    else
    {
        // Raw GI with AO applied
        return float4(gi.rgb * gi.a, 1.0);
    }
}
