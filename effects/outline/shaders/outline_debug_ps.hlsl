// Outline debug shader — visualizes edge detection.
// Red = depth edges (Laplacian), Green = normal edges (Roberts Cross).

#include "outline_normals.hlsl"

Texture2D depthTex   : register(t0);
Texture2D normalsTex : register(t1);

SamplerState pointClamp : register(s0);

cbuffer OutlineParams : register(b0)
{
    float2 texelSize;
    float  depthThreshold;
    float  normalThreshold;
    float  thickness;
    float  strength;
    float  maxDepth;
    float  _pad0;
    float4 outlineColor;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 ox = float2(texelSize.x * thickness, 0.0);
    float2 oy = float2(0.0, texelSize.y * thickness);

    float dC = depthTex.SampleLevel(pointClamp, uv, 0).r;
    if (dC <= 0.0001 || dC > maxDepth)
        return float4(0.0, 0.0, 0.0, 1.0);
    float dL = depthTex.SampleLevel(pointClamp, uv - ox, 0).r;
    float dR = depthTex.SampleLevel(pointClamp, uv + ox, 0).r;
    float dU = depthTex.SampleLevel(pointClamp, uv - oy, 0).r;
    float dD = depthTex.SampleLevel(pointClamp, uv + oy, 0).r;

    float laplacian = abs(dL + dR + dU + dD - 4.0 * dC);
    laplacian /= max(dC, 0.001);
    float depthFactor = smoothstep(depthThreshold, depthThreshold * 1.5, laplacian);

    float2 offset = texelSize * thickness;
    float3 n00 = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float3 n11 = normalsTex.SampleLevel(pointClamp, uv + offset, 0).rgb * 2.0 - 1.0;
    float3 n10 = normalsTex.SampleLevel(pointClamp, uv + float2(offset.x, 0.0), 0).rgb * 2.0 - 1.0;
    float3 n01 = normalsTex.SampleLevel(pointClamp, uv + float2(0.0, offset.y), 0).rgb * 2.0 - 1.0;

    float nEdge = OutlineNormalDifference(n00, n11) + OutlineNormalDifference(n10, n01);
    float normalFactor = smoothstep(normalThreshold, normalThreshold * 1.5 + 1e-5, nEdge);

    return float4(depthFactor, normalFactor, 0.0, 1.0);
}
