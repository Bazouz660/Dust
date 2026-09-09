#pragma once

#include <string>

namespace RTWShadowShader
{
// Embedded in the game deferred shader; no runtime include path is required.
constexpr char ShaderSource[] = R"hlsl(

static const float kDustRtwWarpWidth = 513.0;

// [Dust] Bilinear warp lookup (replacement for point-sampled GetOffsetLocationS)
float DustWarp1D(sampler2D warpMap, float u, float row, out float scale)
{
    float texelPosition = u * kDustRtwWarpWidth - 0.5;
    float leftTexel = clamp(floor(texelPosition), 0.0, kDustRtwWarpWidth - 2.0);
    float blend = saturate(texelPosition - leftTexel);
    float leftUV = (leftTexel + 0.5) / kDustRtwWarpWidth;
    float rightUV = (leftTexel + 1.5) / kDustRtwWarpWidth;
    float leftOffset = tex2Dlod(warpMap, float4(leftUV, row, 0, 0)).x;
    float rightOffset = tex2Dlod(warpMap, float4(rightUV, row, 0, 0)).x;
    scale = 1.0 + (rightOffset - leftOffset) * kDustRtwWarpWidth;
    return lerp(leftOffset, rightOffset, blend);
}
float2 DustGetOffsetLocationS(sampler2D warpMap, float2 uv, out float2 scale)
{
    uv.x += DustWarp1D(warpMap, uv.x, 0.25, scale.x);
    uv.y += DustWarp1D(warpMap, uv.y, 0.75, scale.y);
    return uv;
}

// Fit the geometric receiver plane before the nonlinear warp. Normal maps do
// not describe that plane, and differentiating warped UVs across a whole screen
// pixel gives the wrong slope where the warp changes rapidly.
float2 DustRtwReceiverGradient(float3 shadowPosition)
{
    float3 dx = ddx(shadowPosition);
    float3 dy = ddy(shadowPosition);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    float areaScale = length(dx.xy) * length(dy.xy);
    if (abs(determinant) <= max(areaScale * 1e-5, 1e-20))
    {
        return float2(0, 0);
    }
    float2 gradient = float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / determinant;
    return gradient;
}

// Express each sampled depth at the center of the receiver plane. Correct for
// the actual texel center too: a point sample can be half a texel away from UV.
// This removes filter-induced acne without pushing the entire shadow away.
struct DustRtwReceiver
{
    float3 position;
    float2 warpedUV;
    float2 warpScale;
    float2 gradient;
};

float DustRtwDepth(sampler2D shadowMap, sampler2D warpMap, float2 uv, DustRtwReceiver receiver)
{
    if (any(uv < 0) || any(uv > 1))
    {
        return 1.0;
    }
    // Within the same linear warp segment the center lookup is exact. Reuse
    // it for nearby taps; only taps crossing a segment need more warp reads.
    float2 localScale = receiver.warpScale;
    float2 warpedUV = receiver.warpedUV + (uv - receiver.position.xy) * localScale;
    if (any(floor(uv * kDustRtwWarpWidth - 0.5) !=
            floor(receiver.position.xy * kDustRtwWarpWidth - 0.5)))
    {
        warpedUV = DustGetOffsetLocationS(warpMap, uv, localScale);
    }
    if (any(localScale <= 1e-5))
    {
        return 1.0;
    }
    float2 sampleUV = (floor(warpedUV / dustShadowTexel) + 0.5) * dustShadowTexel;
    if (any(sampleUV < 0) || any(sampleUV > 1))
    {
        return 1.0;
    }
    float depth = tex2Dlod(shadowMap, float4(sampleUV, 0, 0)).x;
    float2 receiverOffset = uv - receiver.position.xy + (sampleUV - warpedUV) / localScale;
    return depth - dot(receiverOffset, receiver.gradient);
}

float DustShadowCmp(sampler2D shadowMap, sampler2D warpMap, float2 uv, float receiverDepth,
                    float bias, DustRtwReceiver receiver)
{
    return DustRtwDepth(shadowMap, warpMap, uv, receiver) >= receiverDepth - bias ? 1.0 : 0.0;
}

// Sample in the original light projection, then warp each tap independently.
// A fixed post-warp radius would change its world footprint whenever the
// camera-dependent importance map redistributes shadow texels.
static const float2 kDustRtwPoisson[12] =
{
    float2(-0.326212, -0.405810),
    float2(-0.840144, -0.073580),
    float2(-0.695914, 0.457137),
    float2(-0.203345, 0.620716),
    float2(0.962340, -0.194983),
    float2(0.473434, -0.480026),
    float2(0.519456, 0.767022),
    float2(0.185461, -0.893124),
    float2(0.507431, 0.064425),
    float2(0.896420, 0.412458),
    float2(-0.321940, -0.932615),
    float2(-0.791559, -0.597705)
};

DustRtwReceiver DustRtwCreateReceiver(sampler2D warpMap, float4x4 shadowMatrix,
                                      float3 worldPosition)
{
    DustRtwReceiver receiver;
    receiver.position = mul(shadowMatrix, float4(worldPosition, 1)).xyz;
    receiver.warpedUV = DustGetOffsetLocationS(warpMap, receiver.position.xy, receiver.warpScale);
    receiver.gradient = DustRtwReceiverGradient(receiver.position);
    return receiver;
}

float DustRtwReceiverBias(DustRtwReceiver receiver, float depthBias, float edgeBias, float3 normal,
                          float cameraDistance, float shadowRange)
{
    float2 edge = saturate(abs(receiver.warpedUV - 0.5) * 20 - 9);
    depthBias += edgeBias * (edge.x + edge.y);

#if DUST_WORKSHOP_STEEP_BIAS
    {
        float verticalNormal = abs(normal.y);
        float steepness = saturate((0.42 - verticalNormal) * 4.25);
        float distanceFade = saturate((cameraDistance - shadowRange * 0.10) * 0.0035);
        depthBias -= (steepness * steepness) * distanceFade * 0.0032;
    }
#endif
    if (dustRtwCliffFixEnabled > 0.5)
    {
        float verticalNormal = abs(normal.y);
        float steepness = saturate((0.42 - verticalNormal) * 4.25);
        float distanceFade =
            saturate((cameraDistance - shadowRange * dustRtwCliffFixDistance) * 0.0035);
        depthBias += (steepness * steepness) * distanceFade * 0.0032;
    }

    return depthBias;
}

float2x2 DustRtwSampleRotation(float2 screenPosition)
{
    float noise = frac(52.9829189 * frac(dot(screenPosition, float2(0.06711056, 0.00583715))));
    float angle = noise * 6.28318530718;
    float sine, cosine;
    sincos(angle, sine, cosine);
    return float2x2(cosine, sine, -sine, cosine);
}

float2 DustRtwFilterRadius(sampler2D shadowMap, sampler2D warpMap, DustRtwReceiver receiver,
                           float4x4 shadowMatrix, float2x2 rotation, float depthBias,
                           float shadowRange)
{
    float receiverDepth = saturate(receiver.position.z);
    // Filter Radius remains a texel-sized antialiasing floor. PCSS Light Size
    // is an angular radius; its world-space footprint is independent of the
    // atlas resolution and the shadow camera's depth origin.
    float2 baseRadius = dustRtwFilterRadius / max(receiver.warpScale, float2(1e-5, 1e-5));
    float2 filterRadius = baseRadius;
    float centerDepth = DustRtwDepth(shadowMap, warpMap, receiver.position.xy, receiver);

    if (dustRtwPcssEnabled > 0.5)
    {
        float depthScale = max(length(shadowMatrix[2].xyz), 1e-8);
        float2 uvScale = float2(length(shadowMatrix[0].xyz), length(shadowMatrix[1].xyz));
        float2 searchRadius = max(baseRadius, shadowRange * dustRtwLightSize * uvScale);
        float blockerSeparation = 0;
        float blockerCount = 0;
        if (centerDepth < receiverDepth - depthBias)
        {
            blockerSeparation = receiverDepth - centerDepth;
            blockerCount = 1;
        }
        // Search all four directions at each scale. A single wide ring can
        // miss every caster after warp clipping; a partial probe cannot prove
        // that the rest of the search footprint is empty.
        static const float2 searchDirections[4] =
        {
            float2(1, 0),
            float2(0, 1),
            float2(-1, 0),
            float2(0, -1)
        };
        static const float searchScales[3] = {0.0625, 0.25, 1.0};
        [unroll]
        for (int j = 0; j < 12; j++)
        {
            float2 radius = max(baseRadius, searchRadius * searchScales[j / 4]);
            float2 uv = receiver.position.xy + mul(rotation, searchDirections[j % 4]) * radius;
            float depth = DustRtwDepth(shadowMap, warpMap, uv, receiver);
            if (depth < receiverDepth - depthBias)
            {
                blockerSeparation += receiverDepth - depth;
                blockerCount += 1;
            }
        }
        if (blockerCount > 0)
        {
            // RTW stores affine directional-light depth, not distance from a
            // point light. Dividing by absolute blocker depth makes softness
            // vary with the shadow camera's near plane. Undo depth scaling only.
            float worldSeparation = blockerSeparation / (blockerCount * depthScale);
            float2 penumbra = worldSeparation * dustRtwLightSize * uvScale;
            filterRadius = max(baseRadius * 0.5, min(penumbra, searchRadius));
        }
    }

    return filterRadius;
}

float DustRtwFilterVisibility(sampler2D shadowMap, sampler2D warpMap, DustRtwReceiver receiver,
                              float2x2 rotation, float2 filterRadius, float depthBias)
{
    float receiverDepth = saturate(receiver.position.z);
    float shadow = 0;

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        float2 uv = receiver.position.xy + mul(rotation, kDustRtwPoisson[i]) * filterRadius;
        shadow += DustShadowCmp(shadowMap, warpMap, uv, receiverDepth, depthBias, receiver);
    }

    float sampleCount = 4.0;
    [branch]
    if (dustRtwQuality > 4.5)
    {
        [unroll]
        for (int i = 4; i < 8; i++)
        {
            float2 uv = receiver.position.xy + mul(rotation, kDustRtwPoisson[i]) * filterRadius;
            shadow += DustShadowCmp(shadowMap, warpMap, uv, receiverDepth, depthBias, receiver);
        }
        sampleCount = 8.0;

        [branch]
        if (dustRtwQuality > 8.5)
        {
            [unroll]
            for (int i = 8; i < 12; i++)
            {
                float2 uv = receiver.position.xy + mul(rotation, kDustRtwPoisson[i]) * filterRadius;
                shadow += DustShadowCmp(shadowMap, warpMap, uv, receiverDepth, depthBias, receiver);
            }
            sampleCount = 12.0;
        }
    }

    shadow /= sampleCount;
    return shadow;
}

float DustRTWShadow(sampler2D shadowMap, sampler2D warpMap, float4x4 shadowMatrix,
                    float3 worldPosition, float depthBias, float edgeBias, float2 screenPosition,
                    float3 normal, float cameraDistance, float shadowRange)
{
    DustRtwReceiver receiver = DustRtwCreateReceiver(warpMap, shadowMatrix, worldPosition);
    float bias =
        DustRtwReceiverBias(receiver, depthBias, edgeBias, normal, cameraDistance, shadowRange);
    float2x2 rotation = DustRtwSampleRotation(screenPosition);
    float2 radius = DustRtwFilterRadius(shadowMap, warpMap, receiver, shadowMatrix, rotation, bias,
                                        shadowRange);
    return DustRtwFilterVisibility(shadowMap, warpMap, receiver, rotation, radius, bias);
}
)hlsl";

inline std::string Source(bool workshopSteepBias)
{
    const char* workshopDefine = workshopSteepBias
        ? "#define DUST_WORKSHOP_STEEP_BIAS 1\n"
        : "#define DUST_WORKSHOP_STEEP_BIAS 0\n";

    return std::string(workshopDefine) + ShaderSource;
}
}
