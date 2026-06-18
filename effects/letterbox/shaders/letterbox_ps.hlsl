Texture2D sceneTex : register(t0);
SamplerState linearClamp : register(s0);

cbuffer LetterboxParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  targetAspect;
    float  opacity;
    float2 _pad;
    float3 barColor;
    int    debugView;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;

    float currentAspect = viewportSize.x * invViewportSize.y;

    if (targetAspect > currentAspect)
    {
        float visibleHeight = currentAspect / targetAspect;
        float barSize = (1.0 - visibleHeight) * 0.5;
        float mask = (uv.y < barSize || uv.y > 1.0 - barSize) ? 1.0 : 0.0;

        color = lerp(color, barColor, mask * opacity);
    }

    return float4(color, 1.0);
}

// Standalone debug overlay: highlights the bar regions. Selected via the
// host's centralized debug-view registry and drawn onto the final LDR target.
float4 debug_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float currentAspect = viewportSize.x * invViewportSize.y;

    float mask = 0.0;
    if (targetAspect > currentAspect)
    {
        float visibleHeight = currentAspect / targetAspect;
        float barSize = (1.0 - visibleHeight) * 0.5;
        mask = (uv.y < barSize || uv.y > 1.0 - barSize) ? 1.0 : 0.0;
    }

    return float4(mask, 1.0 - mask, 0.0, 1.0);
}
