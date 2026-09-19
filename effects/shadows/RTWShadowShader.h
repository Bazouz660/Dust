#pragma once
#include <string>

// Embedded in the game deferred shader; no runtime include path is required.
namespace RTWShadowShader
{
inline std::string Source(bool workshopSteepBias)
{
    return std::string(workshopSteepBias
        ? "#define DUST_WORKSHOP_STEEP_BIAS 1\n"
        : "#define DUST_WORKSHOP_STEEP_BIAS 0\n") + R"hlsl(
// [Dust] Bilinear warp lookup (replacement for point-sampled GetOffsetLocationS)
float DustWarp1D(sampler2D wMap, float u, float row, out float scale) {
    // 513 knots; knot i sits at u = i / 512, as the game's builder places it. Every
    // caster lookup is patched to the same placement (src/RtwWarpLookup.h).
    const float kWarpW = 513.0;
    const float kCells = 512.0;
    float p = u * kCells;
    float pf = clamp(floor(p), 0.0, kCells - 1.0);
    float t = saturate(p - pf);
    float u0 = (pf + 0.5) / kWarpW;
    float u1 = (pf + 1.5) / kWarpW;
    float v0 = tex2Dlod(wMap, float4(u0, row, 0, 0)).x;
    float v1 = tex2Dlod(wMap, float4(u1, row, 0, 0)).x;
    scale = 1.0 + (v1 - v0) * kCells;
    return lerp(v0, v1, t);
}
float2 DustGetOffsetLocationS(sampler2D wMap, float2 ts, out float2 scale) {
    ts.x += DustWarp1D(wMap, ts.x, 0.25, scale.x);
    ts.y += DustWarp1D(wMap, ts.y, 0.75, scale.y);
    return ts;
}

// Fit the geometric receiver plane before the nonlinear warp. Normal maps do
// not describe that plane, and differentiating warped UVs across a whole screen
// pixel gives the wrong slope where the warp changes rapidly.
float2 DustRtwReceiverGradient(float3 shadowPosition) {
    float3 dx = ddx(shadowPosition);
    float3 dy = ddy(shadowPosition);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    float areaScale = length(dx.xy) * length(dy.xy);
    if (abs(determinant) <= max(areaScale * 1e-5, 1e-20))
        return float2(0, 0);
    float2 gradient = float2(dy.y * dx.z - dx.y * dy.z,
                            dx.x * dy.z - dy.x * dx.z) / determinant;
    return gradient;
}

// Express each sampled depth at the center of the receiver plane. Correct for
// the actual texel center too: a point sample can be half a texel away from UV.
// This removes filter-induced acne without pushing the entire shadow away.
struct DustRtwReceiver {
    float3 position;
    float2 warpedUV;
    float2 warpScale;
    float2 gradient;
};

float DustRtwDepth(sampler2D sm, sampler2D wm, float2 uv, DustRtwReceiver receiver) {
    if (any(uv < 0) || any(uv > 1)) return 1.0;
    // Within the same linear warp segment the center lookup is exact. Reuse
    // it for nearby taps; only taps crossing a segment need more warp reads.
    float2 localScale = receiver.warpScale;
    float2 warpedUV = receiver.warpedUV + (uv - receiver.position.xy) * localScale;
    if (any(floor(uv * 512.0) != floor(receiver.position.xy * 512.0)))
        warpedUV = DustGetOffsetLocationS(wm, uv, localScale);
    if (any(localScale <= 1e-5)) return 1.0;
    float2 sampleUV = (floor(warpedUV / dustShadowTexel) + 0.5) * dustShadowTexel;
    if (any(sampleUV < 0) || any(sampleUV > 1)) return 1.0;
    float depth = tex2Dlod(sm, float4(sampleUV, 0, 0)).x;
    float2 receiverOffset = uv - receiver.position.xy + (sampleUV - warpedUV) / localScale;
    return depth - dot(receiverOffset, receiver.gradient);
}

float DustShadowCmp(sampler2D sm, sampler2D wm, float2 uv, float d, float bias,
                    DustRtwReceiver receiver) {
    return DustRtwDepth(sm, wm, uv, receiver) >= d - bias ? 1.0 : 0.0;
}

// Sample in the original light projection, then warp each tap independently.
// A fixed post-warp radius would change its world footprint whenever the
// camera-dependent importance map redistributes shadow texels.
float DustRTWShadow(sampler2D sMap, sampler2D wMap, float4x4 shadowMatrix,
                     float3 worldPos, float b, float edgeBias, float2 screenPos,
                     float3 normal, float dist, float shadowRange) {
    DustRtwReceiver receiver;
    receiver.position = mul(shadowMatrix, float4(worldPos, 1)).xyz;
    receiver.warpedUV = DustGetOffsetLocationS(wMap, receiver.position.xy, receiver.warpScale);
    receiver.gradient = DustRtwReceiverGradient(receiver.position);
    float2 edge = saturate(abs(receiver.warpedUV - 0.5) * 20 - 9);
    b += edgeBias * (edge.x + edge.y);
    float sd = saturate(receiver.position.z);

#if DUST_WORKSHOP_STEEP_BIAS
    float ny = abs(normal.y);
    float steep = saturate((0.42 - ny) * 4.25);
    float farGate = saturate((dist - shadowRange * 0.10) * 0.0035);
    b -= (steep * steep) * farGate * 0.0032;
#endif
    if (dustRtwCliffFixEnabled > 0.5) {
        float cf_ny = abs(normal.y);
        float cf_steep = saturate((0.42 - cf_ny) * 4.25);
        float cf_gate = saturate((dist - shadowRange * dustRtwCliffFixDistance) * 0.0035);
        b += (cf_steep * cf_steep) * cf_gate * 0.0032;
    }

    // Tap rotation. Interleaved gradient noise nearly alternates between neighbouring
    // pixels, which read as a one-pixel checker in penumbrae; the R2 low-discrepancy
    // sequence does not. While a temporal AA integrates frames the host advances
    // dustFrame (golden-ratio steps) so the remaining grain averages out; it is 0
    // otherwise, because a moving pattern would just boil.
    float tapRotation = 6.28318530718 *
        frac(dot(screenPos, float2(0.7548776662, 0.5698402910)) + dustFrame * 0.6180339887);

    // Filter Radius remains a texel-sized antialiasing floor. PCSS Light Size
    // is an angular radius; its world-space footprint is independent of the
    // atlas resolution and the shadow camera's depth origin.
    float2 baseRadius = dustRtwFilterRadius / max(receiver.warpScale, float2(1e-5, 1e-5));
    float2 filterRadius = baseRadius;
    float centerDepth = DustRtwDepth(sMap, wMap, receiver.position.xy, receiver);

    if (dustRtwPcssEnabled > 0.5) {
        float depthScale = max(length(shadowMatrix[2].xyz), 1e-8);
        float2 uvScale = float2(length(shadowMatrix[0].xyz), length(shadowMatrix[1].xyz));
        // A blocker h above the receiver covers part of the light disc only
        // within h * lightSize of it. The widest useful search is therefore
        // the reach of the farthest blocker considered. Shadow Range alone is
        // far too generous a bound for that distance: rings hundreds of units
        // wide sampled unrelated hills and roofs and blurred contact shadows.
        static const float kMaxBlockerDistance = 500.0;
        float maxReach = min(shadowRange, kMaxBlockerDistance) * dustRtwLightSize;
        float2 searchRadius = max(baseRadius, maxReach * uvScale);
        float blockerSeparation = 0;
        float blockerCount = 0;
        if (centerDepth < sd - b) {
            blockerSeparation = sd - centerDepth;
            blockerCount = 1;
        }
        // Six geometric rings of four FIXED directions, alternate rings turned by 45
        // degrees. A per-pixel rotation made neighbouring pixels disagree about whether
        // a blocker exists, so the filter radius flipped between them: the hatching at
        // the lit edge of penumbrae. A surface counts only if it is high enough for its
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
        static const float searchScales[6] = {0.03125, 0.0625, 0.125, 0.25, 0.5, 1.0};
        static const float2 searchDirections[8] = {
            float2( 1.0,  0.0), float2( 0.0,  1.0), float2(-1.0,  0.0), float2( 0.0, -1.0),
            float2( 0.70710678,  0.70710678), float2(-0.70710678,  0.70710678),
            float2(-0.70710678, -0.70710678), float2( 0.70710678, -0.70710678)
        };
        [unroll] for (int j = 0; j < 24; j++) {
            int ring = j / 4;
            float ringReach = maxReach * searchScales[ring];
            float2 radius = max(baseRadius, ringReach * uvScale);
            float2 direction = searchDirections[(ring % 2) * 4 + (j % 4)];
            float depth = DustRtwDepth(sMap, wMap, receiver.position.xy + direction * radius, receiver);
            [branch] if (depth < sd - b) {
                float reach = (sd - depth) / depthScale * dustRtwLightSize;
                bool accepted = all(radius <= baseRadius) || reach >= ringReach;
                [branch] if (!accepted) {
                    float2 inner = max(baseRadius, reach * uvScale);
                    depth = DustRtwDepth(sMap, wMap, receiver.position.xy + direction * inner, receiver);
                    accepted = depth < sd - b;
                }
                if (accepted) {
                    blockerSeparation += sd - depth;
                    blockerCount += 1;
                }
            }
        }
        if (blockerCount > 0) {
            // RTW stores affine directional-light depth, not distance from a
            // point light. Dividing by absolute blocker depth makes softness
            // vary with the shadow camera's near plane. Undo depth scaling only.
            float worldSeparation = blockerSeparation / (blockerCount * depthScale);
            float2 penumbra = worldSeparation * dustRtwLightSize * uvScale;
            filterRadius = max(baseRadius * 0.5, min(penumbra, searchRadius));
        }
    }

    // A Vogel (golden-angle) disk: well spread at any count, and one rotation angle per
    // pixel is enough. The atlas-tier count (4 at 12288) assumes a texel-sized filter;
    // a wide penumbra needs the full disk or it is mostly grain.
    float taps = any(filterRadius > baseRadius * 2.0) ? 24.0 : dustRtwQuality;
    float shadow = 0;
    [loop] for (int i = 0; i < 24; i++) {
        if (i >= (int)taps) break;
        float tapAngle = i * 2.39996323 + tapRotation;
        float2 tap = sqrt((i + 0.5) / taps) * float2(cos(tapAngle), sin(tapAngle));
        shadow += DustShadowCmp(sMap, wMap, receiver.position.xy + tap * filterRadius, sd, b, receiver);
    }
    shadow /= taps;
    return shadow;
}

)hlsl";
}
}
