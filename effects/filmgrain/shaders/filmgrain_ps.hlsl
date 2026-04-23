Texture2D sceneTex : register(t0);
SamplerState linearClamp : register(s0);

cbuffer FilmGrainParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  intensity;
    float  grainSize;
    uint   frameIndex;
    int    colored;
    int    debugView;
    float3 _pad;
};

float hash13(float3 p)
{
    float3 p3 = frac(p * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 hash33(float3 p)
{
    float3 p3 = frac(p * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return frac((p3.xxy + p3.yzz) * p3.zyx);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;

    float2 grainCoord = floor(pos.xy / grainSize);
    float3 seed = float3(grainCoord, (float)frameIndex);

    float3 noise;
    if (colored)
        noise = hash33(seed) - 0.5;
    else
        noise = (hash13(seed) - 0.5).xxx;

    if (debugView)
        return float4(noise + 0.5, 1.0);

    color += noise * intensity;
    return float4(saturate(color), 1.0);
}
