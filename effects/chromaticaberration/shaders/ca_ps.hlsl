Texture2D sceneTex : register(t0);
SamplerState linearClamp : register(s0);

cbuffer CAParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  strength;
    int    debugView;
    float2 _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 dir = uv - 0.5;
    float dist = length(dir);
    float2 offset = dir * dist * strength;

    float r = sceneTex.SampleLevel(linearClamp, uv + offset, 0).r;
    float g = sceneTex.SampleLevel(linearClamp, uv, 0).g;
    float b = sceneTex.SampleLevel(linearClamp, uv - offset, 0).b;

    return float4(r, g, b, 1.0);
}

// Standalone debug overlay: visualizes the offset magnitude as a heatmap.
// Selected via the host's centralized debug-view registry and drawn onto the
// final LDR target.
float4 debug_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 dir = uv - 0.5;
    float dist = length(dir);
    float2 offset = dir * dist * strength;

    float magnitude = length(offset) * viewportSize.x;
    return float4(magnitude, magnitude * 0.5, 0.0, 1.0);
}
