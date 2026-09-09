#pragma once

#include "RTWShadowShader.h"

namespace ShadowShaderSource
{
// PS b7 avoids the game's auto-allocated CSM globals. Keep this layout in sync
// with ShadowCBData in DustShadows.cpp.
constexpr char Parameters[] = R"hlsl(

// [Dust] Shadow filtering parameters (bound by Shadows plugin at b7)
// Layout must match ShadowCBData in effects/shadows/DustShadows.cpp.
cbuffer DustShadowParams : register(b7)
{
    float dustShadowEnabled;
    float dustRtwFilterRadius;
    float dustRtwLightSize;
    float dustRtwPcssEnabled;
    float dustRtwCliffFixEnabled;
    float dustRtwCliffFixDistance;
    float dustCsmFilterRadius;
    float dustCsmBlendEnabled;
    float dustCsmBlendWidth;
    float dustRtwQuality;
    float dustShadowTexel;
    float dustCsmFarSoftness;
};
)hlsl";

constexpr char Cascades[] = R"hlsl(

static const float2 kDustCsmPoisson[12] =
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

float2 DustVogel(int i, int count, float rotOffset)
{
    const float kGolden = 2.399963229728653;
    float r = sqrt((i + 0.5) / (float)count);
    float theta = i * kGolden + rotOffset;
    float s, c;
    sincos(theta, s, c);
    return float2(c, s) * r;
}

float DustPcfBilinear(sampler2D sm, float3 shadowUv, float3x3 sampleProj, float2 offset2D,
                      float texel)
{
    float3 s = shadowUv + mul(sampleProj, float3(offset2D, 0));
    float invT = 1.0 / texel;
    float2 tc = s.xy * invT - 0.5;
    float2 f = frac(tc);
    float2 uv0 = (floor(tc) + 0.5) * texel;
    float d00 = tex2Dlod(sm, float4(uv0, 0, 0)).x;
    float d10 = tex2Dlod(sm, float4(uv0 + float2(texel, 0), 0, 0)).x;
    float d01 = tex2Dlod(sm, float4(uv0 + float2(0, texel), 0, 0)).x;
    float d11 = tex2Dlod(sm, float4(uv0 + float2(texel, texel), 0, 0)).x;
    float4 lit = step(s.zzzz, float4(d00, d10, d01, d11));
    float bottom = lerp(lit.x, lit.y, f.x);
    float top = lerp(lit.z, lit.w, f.x);
    return lerp(bottom, top, f.y);
}

float DustSampleCascade8(sampler2D shadowDepthMap, sampler2D jitterMap, float3 shadowUv,
                         float3x3 sampleProj, float baseRadius)
{
    float2 noise = tex2Dlod(jitterMap, float4(shadowUv.xy * 1024.0, 0, 0)).xy;
    float ang = noise.x * 6.28318530718;

    float2x2 filterScale = float2x2(baseRadius, 0, 0, baseRadius);
    float occ = 0;
    [unroll]
    for (int j = 0; j < 4; j++)
    {
        if (pcfSample(shadowDepthMap, shadowUv, sampleProj, filterScale, DustVogel(j, 16, ang)) < 0)
        {
            occ += 1;
        }
    }
    if (occ <= 0)
    {
        return 1.0;
    }

    float shadow = 0;
    [unroll]
    for (int k = 0; k < 16; k++)
    {
        float2 off = DustVogel(k, 16, ang) * baseRadius;
        shadow += DustPcfBilinear(shadowDepthMap, shadowUv, sampleProj, off, dustShadowTexel);
    }
    return shadow * (1.0 / 16.0);
}

float DustSampleCascade4(sampler2D shadowDepthMap, sampler2D jitterMap, float3 shadowUv,
                         float3x3 sampleProj, float baseRadius)
{
    float2 noise = tex2Dlod(jitterMap, float4(shadowUv.xy * 1024.0, 0, 0)).xy;
    float ang = noise.x * 6.28318530718;

    float2x2 filterScale = float2x2(baseRadius, 0, 0, baseRadius);
    float occ = 0;
    [unroll]
    for (int j = 0; j < 4; j++)
    {
        if (pcfSample(shadowDepthMap, shadowUv, sampleProj, filterScale, DustVogel(j, 8, ang)) < 0)
        {
            occ += 1;
        }
    }
    if (occ <= 0)
    {
        return 1.0;
    }

    float shadow = 0;
    [unroll]
    for (int k = 0; k < 8; k++)
    {
        float2 off = DustVogel(k, 8, ang) * baseRadius;
        shadow += DustPcfBilinear(shadowDepthMap, shadowUv, sampleProj, off, dustShadowTexel);
    }
    return shadow * (1.0 / 8.0);
}

float DustCascadeShadow(float4 shadowParams, float4x4 shadowViewMat,
                        float4 csmScale[SHADOW_MAP_COUNT], float4 csmTrans[SHADOW_MAP_COUNT],
                        float4 csmParams[SHADOW_MAP_COUNT], float4 csmUvBounds[SHADOW_MAP_COUNT],
                        sampler2D shadowDepthMap, sampler2D shadowJitterMap, float4 posWs,
                        float4 posSs, float3 normalWs, out float3 debugColorMask)
{
    debugColorMask = float3(1, 1, 1);
    if (dustShadowEnabled < 0.5)
    {
        return computeShadowMultiplier(shadowParams, shadowViewMat, csmScale, csmTrans, csmParams,
                                       csmUvBounds, shadowDepthMap, shadowJitterMap, posWs, posSs,
                                       normalWs, debugColorMask);
    }

    if (posSs.z > csmParams[SHADOW_MAP_COUNT - 1][0])
    {
        return 1.0;
    }

    int idx = 0;
    [unroll]
    for (int i = 0; i < SHADOW_MAP_COUNT - 1; i++)
    {
        if (posSs.z > csmParams[i][0])
        {
            idx = i + 1;
        }
    }

    float3 normalLs = normalize(mul(shadowViewMat, float4(normalWs, 0)).xyz);
    float3 xDir = float3(1, 0, 0) - normalLs.x * normalLs;
    float3 yDir = float3(0, 1, 0) - normalLs.y * normalLs;
    float3 zDir = float3(0, 0, 1) - normalLs.z * normalLs;
    float3x3 sampleProj = float3x3(xDir, yDir, zDir);

    float3 posLs = mul(shadowViewMat, posWs).xyz;

    float3 shadowUv0 = csmTrans[idx].xyz + csmScale[idx].xyz * posLs;
    float baseRadius0 = csmParams[idx][1] * dustCsmFilterRadius * 0.75;
    if (idx >= SHADOW_MAP_COUNT - 1)
    {
        baseRadius0 *= dustCsmFarSoftness;
    }
    float s0 = (idx >= SHADOW_MAP_COUNT - 1)
                   ? DustSampleCascade4(shadowDepthMap, shadowJitterMap, shadowUv0, sampleProj,
                                        baseRadius0)
                   : DustSampleCascade8(shadowDepthMap, shadowJitterMap, shadowUv0, sampleProj,
                                        baseRadius0);

    float shadowMul = s0;

    if (dustCsmBlendEnabled > 0.5 && idx + 1 < SHADOW_MAP_COUNT)
    {
        float splitFar = csmParams[idx][0];
        float splitNear = (idx > 0) ? csmParams[idx - 1][0] : 0.0;
        float band = max((splitFar - splitNear) * dustCsmBlendWidth, 1e-5);
        float blendT = saturate((posSs.z - (splitFar - band)) / band);
        if (blendT > 0.0)
        {
            float3 shadowUv1 = csmTrans[idx + 1].xyz + csmScale[idx + 1].xyz * posLs;
            float baseRadius1 = csmParams[idx + 1][1] * dustCsmFilterRadius * 0.75;
            if (idx + 1 >= SHADOW_MAP_COUNT - 1)
            {
                baseRadius1 *= dustCsmFarSoftness;
            }
            float s1 = (idx + 1 >= SHADOW_MAP_COUNT - 1)
                           ? DustSampleCascade4(shadowDepthMap, shadowJitterMap, shadowUv1,
                                                sampleProj, baseRadius1)
                           : DustSampleCascade8(shadowDepthMap, shadowJitterMap, shadowUv1,
                                                sampleProj, baseRadius1);
            shadowMul = lerp(s0, s1, blendT);
        }
    }

    float shadowAmbient = shadowParams[2];
    shadowMul = shadowAmbient + shadowMul * (1.0 - shadowAmbient);
    return shadowMul;
}
)hlsl";

inline std::string Build(bool workshopSteepBias)
{
    std::string source = Parameters;
    source += RTWShadowShader::Source(workshopSteepBias);
    source += Cascades;
    return source;
}
}
