// Outline pixel shader — edge detection from depth + normals, composited onto scene.
// Depth: Laplacian (2nd derivative) — rejects smooth gradients at grazing angles.
// Normals: Roberts Cross with hard smoothstep threshold — only sharp edges pass.

Texture2D sceneTex   : register(t0);
Texture2D depthTex   : register(t1);
Texture2D normalsTex : register(t2);

SamplerState linearClamp : register(s0);
SamplerState pointClamp  : register(s1);

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
    float3 scene = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;

    float2 ox = float2(texelSize.x * thickness, 0.0);
    float2 oy = float2(0.0, texelSize.y * thickness);

    // Sample depth in a cross pattern for Laplacian
    float dC = depthTex.SampleLevel(pointClamp, uv, 0).r;
    float dL = depthTex.SampleLevel(pointClamp, uv - ox, 0).r;
    float dR = depthTex.SampleLevel(pointClamp, uv + ox, 0).r;
    float dU = depthTex.SampleLevel(pointClamp, uv - oy, 0).r;
    float dD = depthTex.SampleLevel(pointClamp, uv + oy, 0).r;

    // Skip sky / far objects
    if (dC <= 0.0001 || dC > maxDepth)
        return float4(scene, 1.0);

    // Laplacian: zero on smooth surfaces at any angle,
    // fires only at actual depth discontinuities
    float laplacian = abs(dL + dR + dU + dD - 4.0 * dC);
    laplacian /= max(dC, 0.001);
    // Tight smoothstep — hard cutoff below threshold, sharp ramp above
    float depthFactor = smoothstep(depthThreshold, depthThreshold * 1.5, laplacian);

    // Normal edge detection (Roberts Cross)
    float2 offset = texelSize * thickness;
    float3 n00 = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float3 n11 = normalsTex.SampleLevel(pointClamp, uv + offset, 0).rgb * 2.0 - 1.0;
    float3 n10 = normalsTex.SampleLevel(pointClamp, uv + float2(offset.x, 0.0), 0).rgb * 2.0 - 1.0;
    float3 n01 = normalsTex.SampleLevel(pointClamp, uv + float2(0.0, offset.y), 0).rgb * 2.0 - 1.0;

    float nEdge = (1.0 - dot(n00, n11)) + (1.0 - dot(n10, n01));
    // Tight smoothstep — only sharp normal breaks pass
    float normalFactor = smoothstep(normalThreshold, normalThreshold * 1.5, nEdge);

    // Combine edges (take the stronger signal)
    float edge = max(depthFactor, normalFactor);

    // Apply outline: lerp scene toward outline color
    float3 result = lerp(scene, outlineColor.rgb, edge * strength);

    return float4(result, 1.0);
}
