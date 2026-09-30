#pragma once

#include <string>

namespace RTWShadowShader
{
// Embedded in the game deferred shader; no runtime include path is required.
constexpr char ShaderSource[] = R"hlsl(

// 513 knots; knot i sits at u = i / 512, as the game's builder places it. Every
// caster lookup is patched to the same placement (src/RtwWarpLookup.h).
static const float kDustRtwWarpWidth = 513.0;
static const float kDustRtwWarpCells = 512.0;

// [Dust] Bilinear warp lookup (replacement for point-sampled GetOffsetLocationS)
float DustWarp1D(sampler2D warpMap, float u, float row, out float scale)
{
    float texelPosition = u * kDustRtwWarpCells;
    float leftTexel = clamp(floor(texelPosition), 0.0, kDustRtwWarpCells - 1.0);
    float blend = saturate(texelPosition - leftTexel);
    float leftUV = (leftTexel + 0.5) / kDustRtwWarpWidth;
    float rightUV = (leftTexel + 1.5) / kDustRtwWarpWidth;
    float leftOffset = tex2Dlod(warpMap, float4(leftUV, row, 0, 0)).x;
    float rightOffset = tex2Dlod(warpMap, float4(rightUV, row, 0, 0)).x;
    scale = 1.0 + (rightOffset - leftOffset) * kDustRtwWarpCells;
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
    if (any(floor(uv * kDustRtwWarpCells) != floor(receiver.position.xy * kDustRtwWarpCells)))
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

// Tap rotation. Interleaved gradient noise nearly alternates between neighbouring
// pixels, which read as a one-pixel checker in penumbrae; the R2 low-discrepancy
// sequence does not. While a temporal AA integrates frames the host advances
// dustFrame (golden-ratio steps) so the remaining grain averages out; it is 0
// otherwise, because a moving pattern would just boil.
float DustRtwTapRotation(float2 screenPosition)
{
    return 6.28318530718 * frac(dot(screenPosition, float2(0.7548776662, 0.5698402910)) +
                                dustFrame * 0.6180339887);
}

)hlsl"
// MSVC caps one string literal at about 16 KB; adjacent literals concatenate.
R"hlsl(
// One averaged blocker distance gives one kernel for everything nearby: next to (or
// inside) the wide penumbra of a far caster, a near object's shadow was smeared with
// it. Blockers are split into a near and a far layer, each with its own penumbra. The
// boundary goes in the middle of the widest empty stretch of log2(separation), at least
// two octaves wide; a fixed ratio from the nearest blocker cut through casters whose own
// separations span more than that ratio.

struct DustRtwLayers
{
    float2 baseRadius;
    float2 nearRadius;
    float2 farRadius;
    float split;        // separation (depth units) where the far layer starts; 1e30 = one layer
    float2 uvScale;     // shadow UV per world unit
    float reachPerDepth; // lightSize / depthScale: a blocker's cone radius per unit of separation
    bool anyBlocker;    // false: the search found no blocker, centre included
};

DustRtwLayers DustRtwFindLayers(sampler2D shadowMap, sampler2D warpMap, DustRtwReceiver receiver,
                                float4x4 shadowMatrix, float depthBias, float shadowRange)
{
    float receiverDepth = saturate(receiver.position.z);
    // Filter Radius remains a texel-sized antialiasing floor. PCSS Light Size
    // is an angular radius; its world-space footprint is independent of the
    // atlas resolution and the shadow camera's depth origin.
    float2 baseRadius = dustRtwFilterRadius / max(receiver.warpScale, float2(1e-5, 1e-5));
    DustRtwLayers layers;
    layers.baseRadius = baseRadius;
    layers.nearRadius = baseRadius;
    layers.farRadius = baseRadius;
    layers.split = 1e30;
    layers.anyBlocker = true;   // PCSS off: always filter
    float depthScale = max(length(shadowMatrix[2].xyz), 1e-8);
    float2 uvScale =
        max(float2(length(shadowMatrix[0].xyz), length(shadowMatrix[1].xyz)), 1e-12);
    layers.uvScale = uvScale;
    layers.reachPerDepth = dustRtwLightSize / depthScale;
    float centerDepth = DustRtwDepth(shadowMap, warpMap, receiver.position.xy, receiver);

    if (dustRtwPcssEnabled > 0.5)
    {
        // A blocker h above the receiver covers part of the light disc only
        // within h * lightSize of it, so the widest useful search is the widest
        // penumbra allowed. That is a setting in world units (Max Penumbra), not
        // Shadow Range: rings hundreds of units wide sampled unrelated hills and
        // roofs, and a shadow map holds one surface per texel, so inside a tall
        // caster's wide soft edge the crisp shadows of whatever stands under it are
        // simply absent. A narrow cap keeps that zone a thin strip. An unbound or
        // zero value falls back to the previous 500-unit blocker distance.
        float maxReach = dustRtwMaxPenumbra > 0.0
            ? dustRtwMaxPenumbra
            : min(shadowRange, 500.0) * dustRtwLightSize;
        maxReach = min(maxReach, shadowRange * dustRtwLightSize);
        float2 searchRadius = max(baseRadius, maxReach * uvScale);
        float separations[13];      // per search tap, then the centre; 0 = no blocker
        separations[12] = centerDepth < receiverDepth - depthBias ? receiverDepth - centerDepth : 0;
        // Three geometric rings (1/16, 1/4 and all of the reach) of four FIXED directions,
        // the middle ring turned by 45 degrees. Six rings (24 taps) cost 18% more of the
        // sun pass on a captured frame with no visible difference, since the probe below
        // already recovers each blocker's exact reach. The directions are fixed: a
        // per-pixel rotation made neighbouring pixels disagree about whether a blocker
        // exists, so the filter radius flipped between them: the hatching at the lit
        // edge of penumbrae. A surface counts only if it is high enough for its
        // light cone to reach this receiver; lower surfaces further away cannot occlude
        // the light disc. Rings at the antialiasing floor are exempt.
        //
        // Rings are discrete: a receiver inside a blocker's cone can sit between the last
        // ring that still finds the blocker and the first that is short enough to pass
        // the cone test, and the penumbra then ended in a hard rim on its lit side. So a
        // hit whose reach is shorter than its ring is probed again, in the same
        // direction, at that reach: still a hit means the receiver is inside the cone.
        // (Merely loosening the test let distant casters widen the filter over nearby
        // contact shadows.)
        static const float searchScales[3] = {0.0625, 0.25, 1.0};
        static const float2 searchDirections[8] =
        {
            float2( 1.0,  0.0),
            float2( 0.0,  1.0),
            float2(-1.0,  0.0),
            float2( 0.0, -1.0),
            float2( 0.70710678,  0.70710678),
            float2(-0.70710678,  0.70710678),
            float2(-0.70710678, -0.70710678),
            float2( 0.70710678, -0.70710678)
        };
        // [loop], not [unroll], on this and the layer loops below: unrolled, the search
        // (with its probe fetches) and the layer bookkeeping expanded the sun pass to ~5000
        // instructions, which NVIDIA's Vulkan compiler (DXVK under Proton) spends minutes on:
        // the game froze at load on Linux. The results are identical.
        [loop]
        for (int j = 0; j < 12; j++)
        {
            int ring = j / 4;
            float ringReach = maxReach * searchScales[ring];
            float2 radius = max(baseRadius, ringReach * uvScale);
            float2 direction = searchDirections[(ring % 2) * 4 + (j % 4)];
            float depth = DustRtwDepth(shadowMap, warpMap,
                                       receiver.position.xy + direction * radius, receiver);
            [branch]
            if (depth < receiverDepth - depthBias)
            {
                float reach = (receiverDepth - depth) / depthScale * dustRtwLightSize;
                bool accepted = all(radius <= baseRadius) || reach >= ringReach;
                [branch]
                if (!accepted)
                {
                    float2 inner = max(baseRadius, reach * uvScale);
                    depth = DustRtwDepth(shadowMap, warpMap,
                                         receiver.position.xy + direction * inner, receiver);
                    accepted = depth < receiverDepth - depthBias;
                }
                separations[j] = accepted ? receiverDepth - depth : 0;
            }
            else
            {
                separations[j] = 0;
            }
        }
        uint occupied = 0;
        [loop]
        for (int k = 0; k < 13; k++)
        {
            if (separations[k] > 0)
            {
                // 2^-10 .. 2^13 world units
                float octave = floor(log2(separations[k] / depthScale)) + 10.0;
                occupied |= 1u << (uint)clamp(octave, 0.0, 23.0);
            }
        }
        int firstOctave = 24, lastOctave = -1;
        [loop]
        for (int o = 0; o < 24; o++)
        {
            if ((occupied >> (uint)o) & 1u)
            {
                firstOctave = min(firstOctave, o);
                lastOctave = o;
            }
        }
        int run = 0, runStart = 0, widest = 0, widestStart = 0;
        [loop]
        for (int e = 0; e < 24; e++)
        {
            bool empty = e > firstOctave && e < lastOctave && !((occupied >> (uint)e) & 1u);
            if (empty)
            {
                if (run == 0)
                {
                    runStart = e;
                }
                run++;
                if (run > widest)
                {
                    widest = run;
                    widestStart = runStart;
                }
            }
            else
            {
                run = 0;
            }
        }
        float splitSeparation = 1e30;
        if (widest >= 2)
        {
            splitSeparation = exp2(widestStart + widest * 0.5 - 10.0) * depthScale;
        }
        float nearSum = 0, nearCount = 0, farSum = 0, farCount = 0;
        [loop]
        for (int m = 0; m < 13; m++)
        {
            float separation = separations[m];
            if (separation > 0)
            {
                if (separation < splitSeparation)
                {
                    nearSum += separation;
                    nearCount += 1;
                }
                else
                {
                    farSum += separation;
                    farCount += 1;
                }
            }
        }
        layers.anyBlocker = nearCount + farCount > 0;   // the centre sample included
        // RTW stores affine directional-light depth, not distance from a point light.
        // Dividing by absolute blocker depth makes softness vary with the shadow
        // camera's near plane. Undo depth scaling only.
        if (nearCount > 0)
        {
            float2 penumbra = nearSum / (nearCount * depthScale) * dustRtwLightSize * uvScale;
            layers.nearRadius = max(baseRadius * 0.5, min(penumbra, searchRadius));
        }
        if (farCount > 0)
        {
            float2 penumbra = farSum / (farCount * depthScale) * dustRtwLightSize * uvScale;
            layers.farRadius = max(baseRadius * 0.5, min(penumbra, searchRadius));
            layers.split = splitSeparation;
        }
    }

    return layers;
}

)hlsl"
// MSVC caps one string literal at about 16 KB; adjacent literals concatenate.
R"hlsl(
// Vogel (golden-angle) disk PCF: well spread at any count, and one rotation angle per
// pixel is enough. The atlas-tier count (4 at 12288) assumes a texel-sized filter; a wide
// penumbra needs the full disk or it is mostly grain.
//
// A blocker occludes through a tap only if the tap lies within that blocker's own light
// cone (separation * lightSize, with a x2 tolerance for blockers a little lower than the
// layer's average). Without it a wide kernel, sized for a far caster, also counted every
// low object its taps happened to land on, up to the whole kernel radius away: that
// smeared near shadows into their surroundings, and left a lit outline around them where
// the layered pixels (which exclude them) met the rest. Taps inside the antialiasing floor
// always count. Blockers at or beyond farthestSeparation belong to the far layer.
float DustRtwFilterVisibility(sampler2D shadowMap, sampler2D warpMap, DustRtwReceiver receiver,
                              DustRtwLayers layers, float tapRotation, float2 filterRadius,
                              float depthBias, float farthestSeparation)
{
    float receiverDepth = saturate(receiver.position.z);
    float taps = any(filterRadius > layers.baseRadius * 2.0) ? 24.0 : dustRtwQuality;
    float floorDistance = length(layers.baseRadius / layers.uvScale);
    float visible = 0;

    [loop]
    for (int i = 0; i < 24; i++)
    {
        if (i >= (int)taps)
        {
            break;
        }
        float tapAngle = i * 2.39996323 + tapRotation;
        float2 tap = sqrt((i + 0.5) / taps) * float2(cos(tapAngle), sin(tapAngle)) * filterRadius;
        float separation =
            receiverDepth - DustRtwDepth(shadowMap, warpMap, receiver.position.xy + tap, receiver);
        float tapDistance = length(tap / layers.uvScale);
        bool inCone = separation * layers.reachPerDepth * 2.0 >= tapDistance ||
                      tapDistance <= floorDistance;
        bool occluded = separation > depthBias && separation < farthestSeparation && inCone;
        visible += occluded ? 0.0 : 1.0;
    }

    return visible / taps;
}

float DustRTWShadow(sampler2D shadowMap, sampler2D warpMap, float4x4 shadowMatrix,
                    float3 worldPosition, float depthBias, float edgeBias, float2 screenPosition,
                    float3 normal, float cameraDistance, float shadowRange)
{
    DustRtwReceiver receiver = DustRtwCreateReceiver(warpMap, shadowMatrix, worldPosition);
    float bias =
        DustRtwReceiverBias(receiver, depthBias, edgeBias, normal, cameraDistance, shadowRange);
    DustRtwLayers layers =
        DustRtwFindLayers(shadowMap, warpMap, receiver, shadowMatrix, bias, shadowRange);
    // No blocker anywhere in the search, centre included: fully lit, which is most of
    // the screen. The filter skipped here samples only the antialiasing floor around a
    // centre and innermost ring that found nothing; on a captured frame no pixel changed
    // visibly, and the sun pass got 8% cheaper.
    [branch]
    if (!layers.anyBlocker)
    {
        return 1.0;
    }
    float tapRotation = DustRtwTapRotation(screenPosition);
    // Independent occluders: the light that passes both layers. The near kernel leaves
    // the far layer's blockers to the far kernel; the far kernel needs no lower bound,
    // the cone test already keeps near blockers out of its outer taps.
    float visibility = DustRtwFilterVisibility(shadowMap, warpMap, receiver, layers, tapRotation,
                                               layers.nearRadius, bias, layers.split);
    [branch]
    if (layers.split < 1e29)
    {
        visibility *= DustRtwFilterVisibility(shadowMap, warpMap, receiver, layers,
                                              tapRotation + 1.0, layers.farRadius, bias, 1e30);
    }
    return visibility;
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
