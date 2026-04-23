Texture2D sceneTex : register(t0);
SamplerState linearClamp : register(s0);

cbuffer VignetteParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  strength;
    float  radius;
    float  softness;
    int    shape;
    float  aspectRatio;
    int    debugView;
    float2 _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 d = (uv - 0.5) * 2.0;

    float screenAspect = viewportSize.x * invViewportSize.y;
    float2 adjusted = float2(d.x * screenAspect / aspectRatio, d.y * aspectRatio);

    float dist;
    if (shape == 1)
        dist = max(abs(adjusted.x), abs(adjusted.y));
    else if (shape == 2)
        dist = abs(adjusted.x) + abs(adjusted.y);
    else
        dist = length(adjusted);

    float vig = 1.0 - smoothstep(radius, radius + softness, dist);

    if (debugView)
        return float4(vig.xxx, 1.0);

    float3 color = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    color *= lerp(1.0, vig, strength);
    return float4(color, 1.0);
}
