// Screen-Space Indirect Lighting (SSIL) — computes indirect color bounce from nearby surfaces.
// Samples the lit HDR scene (not raw albedo) so bounced light carries actual surface radiance
// including direct lighting, shadows, and specular.

Texture2D<float> depthTex  : register(t0);
Texture2D        sceneTex  : register(t1);
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
    float4 camRight;
    float4 camUp;
    float4 camForward;
};

static const float PI = 3.14159265;

// Kenshi's G-buffer depth is the Euclidean distance along the pixel ray / farClip (deferred.hlsl:
// viewPos = normalize(ray) * depth * farClip), not view Z. Treating it as z bent every flat surface
// (up to ~20 degrees at Kenshi's FOV) against its true G-buffer normal: false occlusion growing
// toward the screen edges and shifting with camera rotation.
float3 ReconstructViewPos(float2 uv, float depth)
{
    float3 ray = float3((uv.x * 2.0 - 1.0) * aspectRatio * tanHalfFov, (1.0 - uv.y * 2.0) * tanHalfFov, 1.0);
    return ray * (depth * rsqrt(dot(ray, ray)));
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

    // Hemisphere test against the SMOOTH geometric normal (from depth, nearer neighbour per axis so
    // silhouettes don't bend it). The normal-mapped G-buffer normal tilted the hemisphere at every
    // texture bump: bright and dark halos on detailed walls. The G-buffer normal only orients it.
    float3 worldN = normalize(normalsTex.Sample(pointClamp, uv).rgb * 2.0 - 1.0);
    float3 gbufNormal = normalize(float3(dot(worldN, camRight.xyz), dot(worldN, camUp.xyz), -dot(worldN, camForward.xyz)));
    float3 normal;
    {
        float2 px = float2(invViewportSize.x, 0), py = float2(0, invViewportSize.y);
        float dL = depthTex.Sample(pointClamp, uv - px), dR = depthTex.Sample(pointClamp, uv + px);
        float dU = depthTex.Sample(pointClamp, uv - py), dD = depthTex.Sample(pointClamp, uv + py);
        float3 ddxPos = abs(dL - depth) < abs(dR - depth) ? viewPos - ReconstructViewPos(uv - px, dL)
                                                          : ReconstructViewPos(uv + px, dR) - viewPos;
        float3 ddyPos = abs(dU - depth) < abs(dD - depth) ? viewPos - ReconstructViewPos(uv - py, dU)
                                                          : ReconstructViewPos(uv + py, dD) - viewPos;
        // No isfinite: Proton's HLSL compiler (vkd3d) lacks it. A degenerate cross product
        // (flat-on samples, sky neighbours) falls back to the G-buffer normal instead.
        float3 c = cross(ddxPos, ddyPos);
        float lenSq = dot(c, c);
        normal = lenSq > 1e-24 ? c * rsqrt(lenSq) : gbufNormal;
        if (dot(normal, gbufNormal) < 0.0) normal = -normal;
    }

    // Per-pixel rotation
    float noiseAngle = InterleavedGradientNoise(pos.xy) * PI;

    float viewSpaceRadius = ilRadius;
    float screenRadius = viewSpaceRadius / (depth * tanHalfFov * 2.0);
    screenRadius = clamp(screenRadius, min(minScreenRadius, maxScreenRadius), maxScreenRadius);

    int iNumDirs = (int)numDirections;
    int iNumSteps = (int)numSteps;

    float3 indirectLight = float3(0, 0, 0);
    float totalWeight = 0.0;

    [loop]
    for (int dir = 0; dir < iNumDirs; dir++)
    {
        float angle = (float(dir) / numDirections) * PI + noiseAngle;
        float s, c;
        sincos(angle, s, c);
        float2 direction = float2(c, s);

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
                    // Reject samples across depth discontinuities (silhouette edges)
                    float depthDiff = abs(sDepth - depth);
                    if (depthDiff < viewSpaceRadius * 4.0)
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

                                // Sample lit scene radiance at this point
                                float3 sAlbedo = sceneTex.Sample(pointClamp, sUV).rgb;

                                // Weight by visibility angle and distance falloff
                                float weight = cosAngle * falloff * fgAtten;
                                indirectLight += sAlbedo * weight;
                                totalWeight += weight;
                            }
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
                    float depthDiff = abs(sDepth - depth);
                    if (depthDiff < viewSpaceRadius * 4.0)
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

                                float3 sAlbedo = sceneTex.Sample(pointClamp, sUV).rgb;

                                float weight = cosAngle * falloff * fgAtten;
                                indirectLight += sAlbedo * weight;
                                totalWeight += weight;
                            }
                        }
                    }
                }
            }
        }
    }

    // Scale by total sample count — keeps IL proportional to how much nearby
    // geometry occludes the hemisphere, rather than normalizing to average color.
    float totalSamples = numDirections * numSteps * 2.0;
    indirectLight = (indirectLight / totalSamples) * ilStrength * colorBleeding;

    // Depth fade
    float fadeRange = max(ilMaxDepth - depthFadeStart, 0.0001);
    float depthFade = saturate(1.0 - max(depth - depthFadeStart, 0.0) / fadeRange);
    indirectLight *= depthFade;

    return float4(indirectLight, 1.0);
}
