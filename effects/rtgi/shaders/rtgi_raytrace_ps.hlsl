// RTGI Ray Trace Pass
// Casts screen-space rays, marches depth buffer, samples lit scene radiance at hits.
// Uses Reinhard normalization on hit color to preserve hue at high brightness.
//
// Output: RGBA16F — RGB = indirect light (raw), A = occlusion

static const float PI = 3.14159265358979;
static const float TWO_PI = 6.28318530717959;

// ---- Noise ----

float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float2 Hash22(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

// ---- View-space reconstruction ----

float3 ReconstructViewPos(float2 uv, float depth, float thf, float ar)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * ar * thf * depth;
    pos.y = (1.0 - uv.y * 2.0) * thf * depth;
    pos.z = depth;
    return pos;
}

float2 ViewPosToUV(float3 vp, float thf, float ar)
{
    float2 uv;
    uv.x = (vp.x / (vp.z * ar * thf)) * 0.5 + 0.5;
    uv.y = 0.5 - (vp.y / (vp.z * thf)) * 0.5;
    return uv;
}

// ---- Normal reconstruction ----

float3 ReconstructNormal(Texture2D<float> dt, SamplerState ss,
                         float2 uv, float depth, float2 iv,
                         float thf, float ar)
{
    float dR = dt.SampleLevel(ss, uv + float2( iv.x, 0), 0);
    float dL = dt.SampleLevel(ss, uv + float2(-iv.x, 0), 0);
    float dU = dt.SampleLevel(ss, uv + float2(0, -iv.y), 0);
    float dD = dt.SampleLevel(ss, uv + float2(0,  iv.y), 0);

    float3 vp = ReconstructViewPos(uv, depth, thf, ar);
    float3 ddx_, ddy_;
    if (abs(dR - depth) < abs(dL - depth))
        ddx_ = ReconstructViewPos(uv + float2(iv.x, 0), dR, thf, ar) - vp;
    else
        ddx_ = vp - ReconstructViewPos(uv - float2(iv.x, 0), dL, thf, ar);
    if (abs(dD - depth) < abs(dU - depth))
        ddy_ = ReconstructViewPos(uv + float2(0, iv.y), dD, thf, ar) - vp;
    else
        ddy_ = vp - ReconstructViewPos(uv - float2(0, iv.y), dU, thf, ar);
    return normalize(cross(ddx_, ddy_));
}

// ---- Hemisphere sampling ----

float3 CosineHemisphereSample(float2 xi)
{
    float r = sqrt(xi.x);
    float phi = TWO_PI * xi.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - xi.x)));
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    if (n.z < -0.9999999) { t = float3(0, -1, 0); b = float3(-1, 0, 0); return; }
    float a = 1.0 / (1.0 + n.z);
    float d = -n.x * n.y * a;
    t = float3(1.0 - n.x * n.x * a, d, -n.x);
    b = float3(d, 1.0 - n.y * n.y * a, -n.y);
}

float3 HemisphereToNormal(float3 sDir, float3 n)
{
    float3 t, b;
    BuildOrthonormalBasis(n, t, b);
    return normalize(t * sDir.x + b * sDir.y + n * sDir.z);
}

// ---- Color utilities ----

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- Resources ----

Texture2D<float>  depthTex   : register(t0);
Texture2D<float4> sceneTex   : register(t1); // Lit HDR scene
Texture2D<float4> prevGI     : register(t2); // Previous frame GI (multi-bounce)
SamplerState      pointClamp : register(s0);
SamplerState      linearClamp : register(s1);

cbuffer RTGIParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  rayLength;
    float  raySteps;
    float  thickness;
    float  fadeDistance;
    float  bounceIntensity;
    float  aoIntensity;
    float  frameIndex;
    float  raysPerPixel;
    float4x4 inverseView;
};

float4 TraceRay(float2 startUV, float startDepth, float3 rayDirView, int numSteps)
{
    float3 startView = ReconstructViewPos(startUV, startDepth, tanHalfFov, aspectRatio);
    float3 endView = startView + rayDirView * rayLength * startDepth;

    if (endView.z <= 0.0001)
        return float4(0, 0, 0, 0);

    float2 endUV = ViewPosToUV(endView, tanHalfFov, aspectRatio);
    float2 deltaUV = endUV - startUV;
    float endDepth = endView.z;
    float deltaDepth = endDepth - startDepth;
    float stepSize = 1.0 / (float)numSteps;

    [loop]
    for (int i = 1; i <= numSteps; i++)
    {
        float t = (float)i * stepSize;
        float2 sampleUV = startUV + deltaUV * t;

        if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
            return float4(0, 0, 0, 0);

        float rayDepth = startDepth + deltaDepth * t;
        float sceneDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

        if (sceneDepth <= 0.0001)
            continue;

        float depthDiff = rayDepth - sceneDepth;

        if (depthDiff > 0.0 && depthDiff < thickness * startDepth)
        {
            // Refine hit position
            float prevT = (float)(i - 1) * stepSize;
            float hitT = lerp(prevT, t, 0.5);
            float2 hitUV = saturate(startUV + deltaUV * hitT);

            // Sample the lit scene color at the hit point
            float3 sceneColor = sceneTex.SampleLevel(linearClamp, hitUV, 0).rgb;

            // Reinhard normalization: preserves hue while compressing brightness.
            float lum = Luminance(sceneColor);
            float3 hitColor = sceneColor / (1.0 + lum);

            // Multi-bounce feedback
            if (bounceIntensity > 0.0)
            {
                float3 prevBounce = prevGI.SampleLevel(linearClamp, hitUV, 0).rgb;
                float prevLum = Luminance(prevBounce);
                prevBounce = prevBounce / (1.0 + prevLum);
                hitColor += prevBounce * bounceIntensity;
            }

            // Gentle distance fade
            float attenuation = saturate(1.0 - hitT * hitT * hitT);

            // Screen edge fade
            float2 edgeDist = min(hitUV, 1.0 - hitUV);
            float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 10.0);

            // Distance-based AO
            float occlusion = (1.0 - hitT * hitT);

            return float4(hitColor * attenuation * edgeFade, occlusion);
        }
    }

    return float4(0, 0, 0, 0);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (depth <= 0.0001 || depth > fadeDistance)
        return float4(0, 0, 0, 1);

    float3 normal = ReconstructNormal(depthTex, pointClamp, uv, depth, invViewportSize,
                                       tanHalfFov, aspectRatio);

    // Temporal noise — golden ratio sequence for good frame-to-frame coverage
    float temporalNoise = frac(frameIndex * 0.618033988 + Hash13(float3(pos.xy, 0)));
    int iRaysPerPixel = max(1, (int)raysPerPixel);
    int iRaySteps = max(8, (int)raySteps);

    float3 totalIndirect = float3(0, 0, 0);
    float  totalOcclusion = 0.0;

    [loop]
    for (int ray = 0; ray < iRaysPerPixel; ray++)
    {
        // Stratified: golden ratio + per-ray offset + spatial hash
        float rayPhase = frac(temporalNoise + float(ray) * 0.618033988);
        float2 xi = Hash22(pos.xy + float2(rayPhase * 1e3, ray * 127.1));
        xi = frac(xi + rayPhase);

        float3 localDir = CosineHemisphereSample(xi);
        float3 rayDir = HemisphereToNormal(localDir, normal);

        if (dot(rayDir, normal) < 0.0)
            rayDir = -rayDir;

        float4 hitResult = TraceRay(uv, depth, rayDir, iRaySteps);

        totalIndirect += hitResult.rgb;
        totalOcclusion += hitResult.w;
    }

    totalIndirect /= (float)iRaysPerPixel;
    totalOcclusion /= (float)iRaysPerPixel;

    float ao = saturate(1.0 - totalOcclusion * aoIntensity);

    // Depth fade
    float fadeStart = fadeDistance * 0.8;
    float depthFade = saturate(1.0 - max(depth - fadeStart, 0.0) / max(fadeDistance - fadeStart, 0.001));
    totalIndirect *= depthFade;
    ao = lerp(1.0, ao, depthFade);

    return float4(totalIndirect, ao);
}
