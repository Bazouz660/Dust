#pragma once

static const char* g_FullscreenVS = R"(
struct VS_OUTPUT
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
)";

// GTAO — Ground Truth Ambient Occlusion (Jimenez et al. 2016)
// Marches in structured screen-space directions, tracks horizon angles.
static const char* g_SSAOGenPS = R"(
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
};

static const float PI = 3.14159265;
static const int NUM_DIRECTIONS = 12;
static const int NUM_STEPS = 6;

float3 ReconstructViewPos(float2 uv, float depth)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * aspectRatio * tanHalfFov * depth;
    pos.y = (1.0 - uv.y * 2.0) * tanHalfFov * depth;
    pos.z = depth;
    return pos;
}

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

    // Per-pixel rotation using interleaved gradient noise
    float noiseAngle = InterleavedGradientNoise(pos.xy) * PI;

    // View-space AO radius, projected to UV space.
    // Resolution-independent: same view-space radius always covers
    // the same world-space area regardless of screen resolution.
    float viewSpaceRadius = aoRadius;
    float screenRadius = viewSpaceRadius / (depth * tanHalfFov * 2.0);
    screenRadius = clamp(screenRadius, minScreenRadius, maxScreenRadius);

    float ao = 0.0;

    [unroll]
    for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
    {
        float angle = (float(dir) / float(NUM_DIRECTIONS)) * PI + noiseAngle;
        float2 direction = float2(cos(angle), sin(angle));

        float maxCosPos = sin(aoBias);
        float maxCosNeg = sin(aoBias);

        [unroll]
        for (int step = 1; step <= NUM_STEPS; step++)
        {
            float t = float(step) / float(NUM_STEPS);
            float2 offset = direction * screenRadius * t;

            // Positive direction
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
                        float cosH = dot(diff / dist, normal);
                        float falloff = saturate(1.0 - dist / viewSpaceRadius);
                        falloff = pow(falloff, falloffPower);
                        // Foreground rejection
                        float relFG = max(depth - sDepth, 0.0) / (depth + 0.00001);
                        float fgAtten = exp(-relFG * foregroundFade);
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
                    float3 sPos = ReconstructViewPos(sUV, sDepth);
                    float3 diff = sPos - viewPos;
                    float dist = length(diff);
                    if (dist > 0.00001)
                    {
                        float cosH = dot(diff / dist, normal);
                        float falloff = saturate(1.0 - dist / viewSpaceRadius);
                        falloff = pow(falloff, falloffPower);
                        float relFG = max(depth - sDepth, 0.0) / (depth + 0.00001);
                        float fgAtten = exp(-relFG * foregroundFade);
                        cosH *= falloff * fgAtten;
                        maxCosNeg = max(maxCosNeg, cosH);
                    }
                }
            }
        }

        ao += saturate(maxCosPos) + saturate(maxCosNeg);
    }

    ao = (ao / float(NUM_DIRECTIONS * 2)) * aoStrength;
    ao = saturate(1.0 - ao);

    // Depth-based fade (fades from depthFadeStart to aoMaxDepth)
    float fadeRange = max(aoMaxDepth - depthFadeStart, 0.0001);
    float depthFade = saturate(1.0 - max(depth - depthFadeStart, 0.0) / fadeRange);
    ao = lerp(1.0, ao, depthFade);

    return float4(ao, ao, ao, 1.0);
}
)";

// Guided filter blur — pass 1 (positive gather offsets).
static const char* g_SSAOBlurHPS = R"(
Texture2D<float> aoTex     : register(t0);
Texture2D<float> depthTex  : register(t1);
SamplerState     samPoint  : register(s0);

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
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerDepth = depthTex.Sample(samPoint, uv);
    float4 mv = 0;
    float4 ao, depth;

    ao    = aoTex.Gather(samPoint, uv + float2(-0.5, -0.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(-0.5, -0.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(1.5, -0.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(1.5, -0.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(-0.5, 1.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(-0.5, 1.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(1.5, 1.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(1.5, 1.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    mv /= 16.0;

    float depth_var  = mv.y - mv.x * mv.x;
    float covariance = mv.w - mv.x * mv.z;
    float relVar = depth_var / max(centerDepth * centerDepth, 1e-10);
    float epsilon = exp2(relVar > 0.01 ? -12.0 : -30.0);

    float b = covariance / max(depth_var, epsilon);
    float a = mv.z - b * mv.x;
    return float4(saturate(a + b * centerDepth), 0, 0, 1);
}
)";

// Guided filter blur — pass 2 (negative gather offsets).
static const char* g_SSAOBlurVPS = R"(
Texture2D<float> aoTex     : register(t0);
Texture2D<float> depthTex  : register(t1);
SamplerState     samPoint  : register(s0);

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
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float centerDepth = depthTex.Sample(samPoint, uv);
    float4 mv = 0;
    float4 ao, depth;

    ao    = aoTex.Gather(samPoint, uv + float2(0.5, 0.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(0.5, 0.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(-1.5, 0.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(-1.5, 0.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(0.5, -1.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(0.5, -1.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    ao    = aoTex.Gather(samPoint, uv + float2(-1.5, -1.5) * invViewportSize);
    depth = depthTex.Gather(samPoint, uv + float2(-1.5, -1.5) * invViewportSize);
    mv   += float4(dot(depth, 1), dot(depth, depth), dot(ao, 1), dot(ao, depth));

    mv /= 16.0;

    float depth_var  = mv.y - mv.x * mv.x;
    float covariance = mv.w - mv.x * mv.z;
    float relVar = depth_var / max(centerDepth * centerDepth, 1e-10);
    float epsilon = exp2(relVar > 0.01 ? -12.0 : -30.0);

    float b = covariance / max(depth_var, epsilon);
    float a = mv.z - b * mv.x;
    return float4(saturate(a + b * centerDepth), 0, 0, 1);
}
)";

// AO apply — reads scene copy + AO, simple multiply blend.
static const char* g_SSAOApplyPS = R"(
Texture2D<float>  aoTex    : register(t0);
Texture2D         sceneTex : register(t1);
SamplerState      samPoint : register(s0);

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
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float ao = aoTex.Sample(samPoint, uv);
    float3 scene = sceneTex.Sample(samPoint, uv).rgb;

    // In debug mode: only apply AO to middle third, pass through rest
    if (debugMode > 0.5)
    {
        if (uv.x < 0.333 || uv.x > 0.666)
            return float4(scene, 1);
    }

    float3 result = scene * ao;
    return float4(result, 1.0);
}
)";

// AO debug — no-blend pass. Overwrites left third with raw AO visualization.
// Scales output high so auto-exposure can't wash out the contrast.
static const char* g_SSAODebugPS = R"(
Texture2D<float> aoTex     : register(t0);
SamplerState     samLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    if (uv.x >= 0.333)
        discard;

    float ao = aoTex.Sample(samLinear, uv);
    // Scale AO into a low range so auto-exposure amplifies it back up.
    // Preserves the white=unoccluded, dark=occluded look regardless of exposure.
    float v = ao * 0.01;
    return float4(v, v, v, 1.0);
}
)";
