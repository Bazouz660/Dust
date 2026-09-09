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
    const float kWarpW = 513.0;
    float p = u * kWarpW - 0.5;
    float pf = clamp(floor(p), 0.0, kWarpW - 2.0);
    float t = saturate(p - pf);
    float u0 = (pf + 0.5) / kWarpW;
    float u1 = (pf + 1.5) / kWarpW;
    float v0 = tex2Dlod(wMap, float4(u0, row, 0, 0)).x;
    float v1 = tex2Dlod(wMap, float4(u1, row, 0, 0)).x;
    scale = 1.0 + (v1 - v0) * kWarpW;
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
    if (any(floor(uv * 513.0 - 0.5) != floor(receiver.position.xy * 513.0 - 0.5)))
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

    float noise = frac(52.9829189 * frac(dot(screenPos, float2(0.06711056, 0.00583715))));
    float ang = noise * 6.28318530718;
    float sa, ca;
    sincos(ang, sa, ca);
    float2x2 rot = float2x2(ca, sa, -sa, ca);

    static const float2 pd[12] = {
        float2(-0.326212, -0.405810),
        float2(-0.840144, -0.073580),
        float2(-0.695914,  0.457137),
        float2(-0.203345,  0.620716),
        float2( 0.962340, -0.194983),
        float2( 0.473434, -0.480026),
        float2( 0.519456,  0.767022),
        float2( 0.185461, -0.893124),
        float2( 0.507431,  0.064425),
        float2( 0.896420,  0.412458),
        float2(-0.321940, -0.932615),
        float2(-0.791559, -0.597705)
    };

    // Filter Radius remains a texel-sized antialiasing floor. PCSS Light Size
    // is an angular radius; its world-space footprint is independent of the
    // atlas resolution and the shadow camera's depth origin.
    float2 baseRadius = dustRtwFilterRadius / max(receiver.warpScale, float2(1e-5, 1e-5));
    float2 filterRadius = baseRadius;
    float centerDepth = DustRtwDepth(sMap, wMap, receiver.position.xy, receiver);

    if (dustRtwPcssEnabled > 0.5) {
        float depthScale = max(length(shadowMatrix[2].xyz), 1e-8);
        float2 uvScale = float2(length(shadowMatrix[0].xyz), length(shadowMatrix[1].xyz));
        float2 searchRadius = max(baseRadius, shadowRange * dustRtwLightSize * uvScale);
        float blockerSeparation = 0;
        float blockerCount = 0;
        if (centerDepth < sd - b) {
            blockerSeparation = sd - centerDepth;
            blockerCount = 1;
        }
        // Search all four directions at each scale. A single wide ring can
        // miss every caster after warp clipping; a partial probe cannot prove
        // that the rest of the search footprint is empty.
        static const float2 searchDirections[4] = {
            float2(1, 0), float2(0, 1), float2(-1, 0), float2(0, -1)
        };
        static const float searchScales[3] = {0.0625, 0.25, 1.0};
        [unroll] for (int j = 0; j < 12; j++) {
            float2 radius = max(baseRadius, searchRadius * searchScales[j / 4]);
            float2 uv = receiver.position.xy + mul(rot, searchDirections[j % 4]) * radius;
            float depth = DustRtwDepth(sMap, wMap, uv, receiver);
            if (depth < sd - b) {
                blockerSeparation += sd - depth;
                blockerCount += 1;
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

    float shadow = 0;
    [unroll] for (int i = 0; i < 4; i++)
        shadow += DustShadowCmp(sMap, wMap, receiver.position.xy + mul(rot, pd[i]) * filterRadius, sd, b, receiver);
    float sCount = 4.0;
    [branch] if (dustRtwQuality > 4.5) {
        [unroll] for (int i = 4; i < 8; i++)
            shadow += DustShadowCmp(sMap, wMap, receiver.position.xy + mul(rot, pd[i]) * filterRadius, sd, b, receiver);
        sCount = 8.0;
        [branch] if (dustRtwQuality > 8.5) {
            [unroll] for (int i = 8; i < 12; i++)
                shadow += DustShadowCmp(sMap, wMap, receiver.position.xy + mul(rot, pd[i]) * filterRadius, sd, b, receiver);
            sCount = 12.0;
        }
    }
    shadow /= sCount;
    return shadow;
}

)hlsl";
}
}
