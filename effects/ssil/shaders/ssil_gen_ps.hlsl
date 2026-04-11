// Screen-Space Indirect Lighting (SSIL) — computes indirect color bounce from nearby surfaces.
// Uses the same hemisphere sampling approach as GTAO but accumulates albedo-weighted radiance
// instead of occlusion.

Texture2D<float> depthTex   : register(t0);
Texture2D        albedoTex  : register(t1);
Texture2D        normalsTex : register(t2);
SamplerState     pointClamp : register(s0);

cbuffer SSILParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  ilRadius;
    float  ilStrength;
    float  ilBias;
    float  ilMaxDepth;
    float  foregroundFade;
    float  falloffPower;
    float  maxScreenRadius;
    float  minScreenRadius;
    float  depthFadeStart;
    float  colorBleeding;
    float  debugMode;
    float  blurSharpness;
    float  numDirections;
    float  numSteps;
};

static const float PI = 3.14159265;

float3 ReconstructViewPos(float2 uv, float depth)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * aspectRatio * tanHalfFov * depth;
    pos.y = (1.0 - uv.y * 2.0) * tanHalfFov * depth;
    pos.z = depth;
    return pos;
}

float InterleavedGradientNoise(float2 screenPos)
{
    return frac(52.9829189 * frac(0.06711056 * screenPos.x + 0.00583715 * screenPos.y));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, uv);

    if (depth <= 0.0001 || depth > ilMaxDepth)
        return float4(0, 0, 0, 1);

    float3 viewPos = ReconstructViewPos(uv, depth);

    // Normal from depth cross-derivatives (smallest-delta method)
    float depthR = depthTex.Sample(pointClamp, uv + float2( invViewportSize.x, 0));
    float depthL = depthTex.Sample(pointClamp, uv + float2(-invViewportSize.x, 0));
    float depthU = depthTex.Sample(pointClamp, uv + float2(0, -invViewportSize.y));
    float depthD = depthTex.Sample(pointClamp, uv + float2(0,  invViewportSize.y));

    float3 ddxPos, ddyPos;
    if (abs(depthR - depth) < abs(depthL - depth))
        ddxPos = ReconstructViewPos(uv + float2(invViewportSize.x, 0), depthR) - viewPos;
    else
        ddxPos = viewPos - ReconstructViewPos(uv - float2(invViewportSize.x, 0), depthL);

    if (abs(depthD - depth) < abs(depthU - depth))
        ddyPos = ReconstructViewPos(uv + float2(0, invViewportSize.y), depthD) - viewPos;
    else
        ddyPos = viewPos - ReconstructViewPos(uv - float2(0, invViewportSize.y), depthU);

    float3 normal = normalize(cross(ddxPos, ddyPos));

    // Per-pixel rotation
    float noiseAngle = InterleavedGradientNoise(pos.xy) * PI;

    float viewSpaceRadius = ilRadius;
    float screenRadius = viewSpaceRadius / (depth * tanHalfFov * 2.0);
    screenRadius = clamp(screenRadius, minScreenRadius, maxScreenRadius);

    int iNumDirs = (int)numDirections;
    int iNumSteps = (int)numSteps;

    float3 indirectLight = float3(0, 0, 0);
    float totalWeight = 0.0;

    [loop]
    for (int dir = 0; dir < iNumDirs; dir++)
    {
        float angle = (float(dir) / numDirections) * PI + noiseAngle;
        float2 direction = float2(cos(angle), sin(angle));

        [loop]
        for (int step = 1; step <= iNumSteps; step++)
        {
            float t = float(step) / numSteps;
            float2 offset = direction * screenRadius * t;

            // Sample in positive direction
            {
                float2 sUV = saturate(uv + offset);
                float sDepth = depthTex.Sample(pointClamp, sUV);
                if (sDepth > 0.0001)
                {
                    float3 sPos = ReconstructViewPos(sUV, sDepth);
                    float3 diff = sPos - viewPos;
                    float dist = length(diff);
                    if (dist > 0.00001)
                    {
                        float3 sDir = diff / dist;
                        float cosAngle = dot(sDir, normal);

                        // Only accept contributions from the hemisphere facing the normal
                        if (cosAngle > ilBias)
                        {
                            float falloff = saturate(1.0 - dist / viewSpaceRadius);
                            falloff = pow(falloff, falloffPower);

                            // Foreground rejection
                            float relFG = max(depth - sDepth, 0.0) / (depth + 0.00001);
                            float fgAtten = exp(-relFG * foregroundFade);

                            // Decode sample albedo from GBuffer
                            float3 sAlbedo = albedoTex.Sample(pointClamp, sUV).rgb;

                            // Weight by visibility angle and distance falloff
                            float weight = cosAngle * falloff * fgAtten;
                            indirectLight += sAlbedo * weight;
                            totalWeight += weight;
                        }
                    }
                }
            }

            // Sample in negative direction
            {
                float2 sUV = saturate(uv - offset);
                float sDepth = depthTex.Sample(pointClamp, sUV);
                if (sDepth > 0.0001)
                {
                    float3 sPos = ReconstructViewPos(sUV, sDepth);
                    float3 diff = sPos - viewPos;
                    float dist = length(diff);
                    if (dist > 0.00001)
                    {
                        float3 sDir = diff / dist;
                        float cosAngle = dot(sDir, normal);

                        if (cosAngle > ilBias)
                        {
                            float falloff = saturate(1.0 - dist / viewSpaceRadius);
                            falloff = pow(falloff, falloffPower);

                            float relFG = max(depth - sDepth, 0.0) / (depth + 0.00001);
                            float fgAtten = exp(-relFG * foregroundFade);

                            float3 sAlbedo = albedoTex.Sample(pointClamp, sUV).rgb;

                            float weight = cosAngle * falloff * fgAtten;
                            indirectLight += sAlbedo * weight;
                            totalWeight += weight;
                        }
                    }
                }
            }
        }
    }

    // Normalize and apply strength
    if (totalWeight > 0.0001)
        indirectLight = (indirectLight / totalWeight) * ilStrength * colorBleeding;
    else
        indirectLight = float3(0, 0, 0);

    // Depth fade
    float fadeRange = max(ilMaxDepth - depthFadeStart, 0.0001);
    float depthFade = saturate(1.0 - max(depth - depthFadeStart, 0.0) / fadeRange);
    indirectLight *= depthFade;

    return float4(indirectLight, 1.0);
}
