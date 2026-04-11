// GTAO — Ground Truth Ambient Occlusion (Jimenez et al. 2016)
// Marches in structured screen-space directions, tracks horizon angles.

Texture2D<float> depthTex   : register(t0);
SamplerState     pointClamp : register(s0);

cbuffer SSAOParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  filterRadius;
    float  debugMode;
    float  aoRadius;
    float  aoStrength;
    float  aoBias;
    float  aoMaxDepth;
    float  foregroundFade;
    float  falloffPower;
    float  maxScreenRadius;
    float  minScreenRadius;
    float  depthFadeStart;
    float  blurSharpness;
    float  nightCompensation;
    float  noiseScale;
    float  numDirections;
    float  numSteps;
};

static const float PI = 3.14159265;

// Interleaved gradient noise — better spatial distribution than sin hash
float InterleavedGradientNoise(float2 screenPos)
{
    return frac(52.9829189 * frac(0.06711056 * screenPos.x + 0.00583715 * screenPos.y));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.Sample(pointClamp, uv);

    if (depth <= 0.0001 || depth > aoMaxDepth)
        return float4(1, 1, 1, 1);

    // Precompute UV-to-view-space scale factors (constant across all samples)
    float2 uvScale = float2(aspectRatio * tanHalfFov, tanHalfFov);

    // Center view position
    float3 viewPos;
    viewPos.x = (uv.x * 2.0 - 1.0) * uvScale.x * depth;
    viewPos.y = (1.0 - uv.y * 2.0) * uvScale.y * depth;
    viewPos.z = depth;

    // Normal from depth cross-derivatives (smallest-delta method)
    float depthR = depthTex.Sample(pointClamp, uv + float2( invViewportSize.x, 0));
    float depthL = depthTex.Sample(pointClamp, uv + float2(-invViewportSize.x, 0));
    float depthU = depthTex.Sample(pointClamp, uv + float2(0, -invViewportSize.y));
    float depthD = depthTex.Sample(pointClamp, uv + float2(0,  invViewportSize.y));

    // Use precomputed UV offsets for neighbor reconstruction
    float3 ddxPos, ddyPos;
    if (abs(depthR - depth) < abs(depthL - depth))
    {
        float2 nUV = uv + float2(invViewportSize.x, 0);
        ddxPos = float3((nUV.x * 2.0 - 1.0) * uvScale.x * depthR,
                        (1.0 - nUV.y * 2.0) * uvScale.y * depthR,
                        depthR) - viewPos;
    }
    else
    {
        float2 nUV = uv - float2(invViewportSize.x, 0);
        ddxPos = viewPos - float3((nUV.x * 2.0 - 1.0) * uvScale.x * depthL,
                                   (1.0 - nUV.y * 2.0) * uvScale.y * depthL,
                                   depthL);
    }

    if (abs(depthD - depth) < abs(depthU - depth))
    {
        float2 nUV = uv + float2(0, invViewportSize.y);
        ddyPos = float3((nUV.x * 2.0 - 1.0) * uvScale.x * depthD,
                        (1.0 - nUV.y * 2.0) * uvScale.y * depthD,
                        depthD) - viewPos;
    }
    else
    {
        float2 nUV = uv - float2(0, invViewportSize.y);
        ddyPos = viewPos - float3((nUV.x * 2.0 - 1.0) * uvScale.x * depthU,
                                   (1.0 - nUV.y * 2.0) * uvScale.y * depthU,
                                   depthU);
    }

    float3 normal = normalize(cross(ddxPos, ddyPos));

    // Per-pixel rotation using interleaved gradient noise
    float noiseAngle = InterleavedGradientNoise(pos.xy * noiseScale) * PI;

    // View-space AO radius, projected to UV space
    float viewSpaceRadius = aoRadius;
    float screenRadius = viewSpaceRadius / (depth * tanHalfFov * 2.0);
    screenRadius = clamp(screenRadius, minScreenRadius, maxScreenRadius);

    // Precompute loop invariants
    float invRadius = 1.0 / viewSpaceRadius;
    float biasVal = sin(aoBias);
    float invDirCount = 1.0 / numDirections;
    float invStepCount = 1.0 / numSteps;
    float invDepth = 1.0 / (depth + 0.00001);

    float ao = 0.0;
    int iNumDirs = (int)numDirections;
    int iNumSteps = (int)numSteps;

    [loop]
    for (int dir = 0; dir < iNumDirs; dir++)
    {
        float angle = (float(dir) * invDirCount) * PI + noiseAngle;
        float2 direction = float2(cos(angle), sin(angle));

        float maxCosPos = biasVal;
        float maxCosNeg = biasVal;

        [loop]
        for (int step = 1; step <= iNumSteps; step++)
        {
            float2 offset = direction * (screenRadius * (float(step) * invStepCount));

            // Positive direction
            {
                float2 sUV = saturate(uv + offset);
                float sDepth = depthTex.Sample(pointClamp, sUV);
                if (sDepth > 0.0001)
                {
                    float3 diff = float3(
                        (sUV.x * 2.0 - 1.0) * uvScale.x * sDepth - viewPos.x,
                        (1.0 - sUV.y * 2.0) * uvScale.y * sDepth - viewPos.y,
                        sDepth - viewPos.z);
                    float distSq = dot(diff, diff);
                    if (distSq > 1e-10)
                    {
                        float invDist = rsqrt(distSq);
                        float cosH = dot(diff * invDist, normal);
                        float dist = distSq * invDist;
                        float falloff = saturate(1.0 - dist * invRadius);
                        falloff = pow(falloff, falloffPower);
                        float fgAtten = exp(-max(depth - sDepth, 0.0) * invDepth * foregroundFade);
                        cosH *= falloff * fgAtten;
                        maxCosPos = max(maxCosPos, cosH);
                    }
                }
            }

            // Negative direction
            {
                float2 sUV = saturate(uv - offset);
                float sDepth = depthTex.Sample(pointClamp, sUV);
                if (sDepth > 0.0001)
                {
                    float3 diff = float3(
                        (sUV.x * 2.0 - 1.0) * uvScale.x * sDepth - viewPos.x,
                        (1.0 - sUV.y * 2.0) * uvScale.y * sDepth - viewPos.y,
                        sDepth - viewPos.z);
                    float distSq = dot(diff, diff);
                    if (distSq > 1e-10)
                    {
                        float invDist = rsqrt(distSq);
                        float cosH = dot(diff * invDist, normal);
                        float dist = distSq * invDist;
                        float falloff = saturate(1.0 - dist * invRadius);
                        falloff = pow(falloff, falloffPower);
                        float fgAtten = exp(-max(depth - sDepth, 0.0) * invDepth * foregroundFade);
                        cosH *= falloff * fgAtten;
                        maxCosNeg = max(maxCosNeg, cosH);
                    }
                }
            }
        }

        ao += saturate(maxCosPos) + saturate(maxCosNeg);
    }

    ao = (ao * invDirCount * 0.5) * aoStrength;
    ao = saturate(1.0 - ao);

    // Depth-based fade (fades from depthFadeStart to aoMaxDepth)
    float fadeRange = max(aoMaxDepth - depthFadeStart, 0.0001);
    float depthFade = saturate(1.0 - max(depth - depthFadeStart, 0.0) / fadeRange);
    ao = lerp(1.0, ao, depthFade);

    return float4(ao, ao, ao, 1.0);
}
