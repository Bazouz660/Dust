Texture2D sceneTex : register(t0);
Texture2D depthTex : register(t1);
SamplerState linearClamp : register(s0);

cbuffer DebandParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  threshold;
    float  range;
    float  intensity;
    float  skyDepthThreshold;
    uint   frameIndex;
    int    debugView;
    int    skyOnly;
    float  _pad;
};

float hash13(float3 p)
{
    float3 p3 = frac(p * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;

    if (skyOnly)
    {
        float depth = depthTex.SampleLevel(linearClamp, uv, 0).r;
        bool isSky = depth >= skyDepthThreshold || depth <= 0.0001;

        if (debugView)
        {
            if (isSky)
                return float4(0.0, 0.5, 1.0, 1.0);
            else
                return float4(color * 0.3, 1.0);
        }

        if (!isSky)
            return float4(color, 1.0);
    }

    float3 seed = float3(pos.xy, (float)frameIndex);
    float angle = hash13(seed) * 6.28318;
    float dist = hash13(seed + 50.0) * range;

    float2 offset = float2(cos(angle), sin(angle)) * dist * invViewportSize;
    float3 ref = sceneTex.SampleLevel(linearClamp, uv + offset, 0).rgb;

    float3 diff = abs(color - ref);
    float3 avg = (color + ref) * 0.5;

    float3 result;
    result.r = (diff.r < threshold) ? lerp(color.r, avg.r, intensity) : color.r;
    result.g = (diff.g < threshold) ? lerp(color.g, avg.g, intensity) : color.g;
    result.b = (diff.b < threshold) ? lerp(color.b, avg.b, intensity) : color.b;

    return float4(result, 1.0);
}
