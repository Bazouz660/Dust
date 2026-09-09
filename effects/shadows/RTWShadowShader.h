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
float2 DustRtwReceiverGradient(float3 shadowPosition, float2 warpScale) {
    float3 dx = ddx(shadowPosition);
    float3 dy = ddy(shadowPosition);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    float areaScale = length(dx.xy) * length(dy.xy);
    if (abs(determinant) <= max(areaScale * 1e-5, 1e-20) || any(warpScale <= 1e-5))
        return float2(0, 0);
    float2 gradient = float2(dy.y * dx.z - dx.y * dy.z,
                            dx.x * dy.z - dy.x * dx.z) / determinant;
    return gradient / warpScale;
}

// Express each sampled depth at the center of the receiver plane. Correct for
// the actual texel center too: a point sample can be half a texel away from UV.
// This removes filter-induced acne without pushing the entire shadow away.
float DustRtwDepth(sampler2D sm, float2 uv, float2 center, float2 gradient) {
    float2 sampleUV = (floor(uv / dustShadowTexel) + 0.5) * dustShadowTexel;
    if (any(sampleUV < 0) || any(sampleUV > 1)) return 1.0;
    float depth = tex2Dlod(sm, float4(sampleUV, 0, 0)).x;
    return depth - dot(sampleUV - center, gradient);
}

float DustShadowCmp(sampler2D sm, float2 uv, float d, float bias,
                    float2 center, float2 gradient) {
    return DustRtwDepth(sm, uv, center, gradient) >= d - bias ? 1.0 : 0.0;
}

// [Dust] Improved RTWSM shadow filtering (post-warp offsets)
float DustRTWShadow(sampler2D sMap, sampler2D wMap, float4x4 shadowMatrix,
                     float3 worldPos, float b, float edgeBias, float2 screenPos,
                     float3 normal, float dist, float shadowRange) {
    float3 ld = normalize(shadowMatrix[2].xyz);
    float NdotL = abs(dot(normal, ld));

    float4 sc = mul(shadowMatrix, float4(worldPos, 1));
    float2 warpScale;
    float2 center = DustGetOffsetLocationS(wMap, sc.xy, warpScale);
    float2 receiverGradient = DustRtwReceiverGradient(sc.xyz, warpScale);
    float2 edge = saturate(abs(center - 0.5) * 20 - 9);
    b += edgeBias * (edge.x + edge.y);
    float sd = saturate(sc.z);

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

    float fr = dustRtwFilterRadius;
    float ls = dustRtwLightSize;

    float centerD = DustRtwDepth(sMap, center, center, receiverGradient);
    [branch] if (centerD >= sd - b) {
        float d0 = DustRtwDepth(sMap, center + mul(rot, pd[0]) * fr, center, receiverGradient);
        float d6 = DustRtwDepth(sMap, center + mul(rot, pd[6]) * fr, center, receiverGradient);
        if (d0 >= sd - b && d6 >= sd - b) return 1.0;
    }

    if (dustRtwPcssEnabled > 0.5) {
        float bSum = 0;
        float bCnt = 0;
        [unroll]
        for (int j = 0; j < 4; j++) {
            float dd = DustRtwDepth(sMap, center + mul(rot, pd[j]) * ls, center, receiverGradient);
            if (dd < sd - b) { bSum += dd; bCnt += 1.0; }
        }
        [branch] if (bCnt > 0) {
            [unroll]
            for (int j = 4; j < 12; j++) {
                float dd = DustRtwDepth(sMap, center + mul(rot, pd[j]) * ls, center, receiverGradient);
                if (dd < sd - b) { bSum += dd; bCnt += 1.0; }
            }
            float avgB = bSum / bCnt;
            float pen = (sd - avgB) * ls / max(avgB, 0.001);
            fr = clamp(pen, fr * 0.5, fr * 3.0);
        }
    }

    fr *= max(sqrt(NdotL), 0.15);

    float shadow = 0;
    [unroll] for (int i = 0; i < 4; i++)
        shadow += DustShadowCmp(sMap, center + mul(rot, pd[i]) * fr, sd, b, center, receiverGradient);
    float sCount = 4.0;
    [branch] if (dustRtwQuality > 4.5) {
        [unroll] for (int i = 4; i < 8; i++)
            shadow += DustShadowCmp(sMap, center + mul(rot, pd[i]) * fr, sd, b, center, receiverGradient);
        sCount = 8.0;
        [branch] if (dustRtwQuality > 8.5) {
            [unroll] for (int i = 8; i < 12; i++)
                shadow += DustShadowCmp(sMap, center + mul(rot, pd[i]) * fr, sd, b, center, receiverGradient);
            sCount = 12.0;
        }
    }
    shadow /= sCount;
    return shadow;
}

)hlsl";
}
}
