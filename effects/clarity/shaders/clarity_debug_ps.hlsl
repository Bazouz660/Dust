// Debug visualization for Clarity — shows the extracted detail layer.
// Gray (0.5) = no detail, brighter = positive detail, darker = negative detail.

Texture2D sceneTex       : register(t0);
Texture2D blurTex        : register(t1);
SamplerState linearClamp : register(s0);

cbuffer ClarityParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  strength;
    float  midtoneProtect;
    float  blurRadius;
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 original = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 blurred  = blurTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 detail   = original - blurred;

    // Visualize: 0.5 + detail * strength, so gray = no detail
    float3 vis = 0.5 + detail * strength;
    return float4(saturate(vis), 1.0);
}
