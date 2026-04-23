// RTGI Ray Trace — Compute Shader
// Compute eliminates pixel shader 2×2 quad helper lane waste at depth
// discontinuities, where 1-3 threads per quad produce no useful output.

static const float PI = 3.14159265358979;
static const float TWO_PI = 6.28318530717959;

float IGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

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

float3 CosineHemisphereSample(float2 xi)
{
    float r = sqrt(xi.x);
    float phi = TWO_PI * xi.y;
    float s, c;
    sincos(phi, s, c);
    return float3(r * c, r * s, sqrt(max(0.0, 1.0 - xi.x)));
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    if (n.z < -0.9999999) { t = float3(0, -1, 0); b = float3(-1, 0, 0); return; }
    float a = 1.0 / (1.0 + n.z);
    float d = -n.x * n.y * a;
    t = float3(1.0 - n.x * n.x * a, d, -n.x);
    b = float3(d, 1.0 - n.y * n.y * a, -n.y);
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

Texture2D<float>    depthTex    : register(t0);
Texture2D<float4>   sceneTex    : register(t1);
Texture2D<float4>   prevGI      : register(t2);
Texture2D<float4>   normalsTex  : register(t3);
RWTexture2D<float4> outTex      : register(u0);
SamplerState        pointClamp  : register(s0);
SamplerState        linearClamp : register(s1);

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
    float  thicknessCurve;
    float  normalDetail;
    float2 sampleJitter;
    float2 _padJitter;
    float4 camRight;
    float4 camUp;
    float4 camForward;
};

float4 TraceRay(float2 startUV, float startDepth, float3 startView, float3 rayDirView, int numSteps, float thicknessLimit)
{
    float3 endView = startView + rayDirView * rayLength * startDepth;

    if (endView.z <= 0.0001)
        return float4(0, 0, 0, 0);

    float2 endUV = ViewPosToUV(endView, tanHalfFov, aspectRatio);
    float2 deltaUV = endUV - startUV;
    float endDepth = endView.z;
    float deltaDepth = endDepth - startDepth;
    float invSteps = 1.0 / (float)numSteps;

    [loop]
    for (int i = 1; i <= numSteps; i++)
    {
        // Non-linear step distribution: pow(1.5) clusters more samples near the
        // ray origin where nearby geometry contributes the most indirect light.
        float tLinear = (float)i * invSteps;
        float t = tLinear * sqrt(tLinear);

        float2 sampleUV = startUV + deltaUV * t;

        if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
            return float4(0, 0, 0, 0);

        float rayDepth = startDepth + deltaDepth * t;
        float sceneDepth = depthTex.SampleLevel(pointClamp, sampleUV, 0);

        if (sceneDepth <= 0.0001)
            continue;

        float depthDiff = rayDepth - sceneDepth;

        if (depthDiff > 0.0 && depthDiff < thicknessLimit)
        {
            float hitT = t;

            if (numSteps >= 8)
            {
                float tLoLinear = (float)(i - 1) * invSteps;
                float tLo = tLoLinear * sqrt(tLoLinear);
                float tHi = t;
                [unroll]
                for (int j = 0; j < 4; j++)
                {
                    float tMid = (tLo + tHi) * 0.5;
                    float midRayD = startDepth + deltaDepth * tMid;
                    float midSceneD = depthTex.SampleLevel(pointClamp, startUV + deltaUV * tMid, 0);
                    if (midSceneD > 0.0001 && midRayD > midSceneD)
                        tHi = tMid;
                    else
                        tLo = tMid;
                }
                hitT = tHi;
            }

            float2 hitUV = saturate(startUV + deltaUV * hitT);
            float3 sceneColor = sceneTex.SampleLevel(linearClamp, hitUV, 0).rgb;
            float lum = Luminance(sceneColor);
            float compress = 1.0 / (1.0 + lum);
            float3 hitColor = sceneColor * compress * compress;

            if (bounceIntensity > 0.0)
            {
                float3 prevBounce = prevGI.SampleLevel(linearClamp, hitUV, 0).rgb;
                float prevLum = Luminance(prevBounce);
                float prevCompress = 1.0 / (1.0 + prevLum);
                prevBounce = prevBounce * prevCompress * prevCompress;
                hitColor += prevBounce * bounceIntensity;
            }

            // Attenuate shallow hits: on smooth surfaces (terrain), the ray grazes
            // the starting surface with depthDiff barely above zero.  These marginal
            // hits toggle frame-to-frame and cause flicker.  Real geometry intersections
            // penetrate deeper into the thickness zone.
            float penetration = smoothstep(0.0, 0.3, depthDiff / thicknessLimit);

            float attenuation = saturate(1.0 - hitT * hitT * hitT);
            float2 edgeDist = min(hitUV, 1.0 - hitUV);
            float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 10.0);
            float occlusion = (1.0 - hitT * hitT);

            return float4(hitColor * attenuation * edgeFade * penetration, occlusion * penetration);
        }
    }

    return float4(0, 0, 0, 0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)viewportSize.x || tid.y >= (uint)viewportSize.y)
        return;

    float2 pixelPos = float2(tid.xy) + 0.5;
    float2 uv = pixelPos * invViewportSize;

    uv += sampleJitter * invViewportSize;

    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (depth <= 0.0001 || depth > fadeDistance)
    {
        outTex[tid.xy] = float4(0, 0, 0, 1);
        return;
    }

    float3 startView = ReconstructViewPos(uv, depth, tanHalfFov, aspectRatio);

    float3 geoNormal;
    {
        float dL = depthTex.SampleLevel(pointClamp, uv - float2(invViewportSize.x, 0), 0);
        float dR = depthTex.SampleLevel(pointClamp, uv + float2(invViewportSize.x, 0), 0);
        float dU = depthTex.SampleLevel(pointClamp, uv - float2(0, invViewportSize.y), 0);
        float dD = depthTex.SampleLevel(pointClamp, uv + float2(0, invViewportSize.y), 0);

        float3 ddx_pos = (abs(dL - depth) < abs(dR - depth))
            ? startView - ReconstructViewPos(uv - float2(invViewportSize.x, 0), dL, tanHalfFov, aspectRatio)
            : ReconstructViewPos(uv + float2(invViewportSize.x, 0), dR, tanHalfFov, aspectRatio) - startView;
        float3 ddy_pos = (abs(dU - depth) < abs(dD - depth))
            ? startView - ReconstructViewPos(uv - float2(0, invViewportSize.y), dU, tanHalfFov, aspectRatio)
            : ReconstructViewPos(uv + float2(0, invViewportSize.y), dD, tanHalfFov, aspectRatio) - startView;

        geoNormal = normalize(cross(ddx_pos, ddy_pos));
    }

    float3 worldN = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float3 gbufNormal;
    gbufNormal.x =  dot(worldN, camRight.xyz);
    gbufNormal.y =  dot(worldN, camUp.xyz);
    gbufNormal.z = -dot(worldN, camForward.xyz);
    gbufNormal = normalize(gbufNormal);

    if (dot(geoNormal, gbufNormal) < 0)
        geoNormal = -geoNormal;

    float3 normal = normalize(lerp(geoNormal, gbufNormal, normalDetail));
    float3 tangent, bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);

    int iRaysPerPixel = max(1, (int)raysPerPixel);
    int iRaySteps = max(1, (int)raySteps);

    float thicknessLimit = thickness * pow(depth, thicknessCurve);

    float2 jitter = float2(frac(frameIndex * 0.7548776662), frac(frameIndex * 0.5698402910)) * 127.0;
    float2 blueNoise = float2(IGN(pixelPos + jitter), IGN(pixelPos + jitter + float2(23.97, 47.31)));
    float temporalPhase = frac(frameIndex * 0.618033988);

    float3 totalIndirect = float3(0, 0, 0);
    float  totalOcclusion = 0.0;

    [loop]
    for (int ray = 0; ray < iRaysPerPixel; ray++)
    {
        float rayPhase = frac(temporalPhase + float(ray) * 0.618033988);
        float2 xi = frac(blueNoise + rayPhase);

        float3 localDir = CosineHemisphereSample(xi);
        float3 rayDir = tangent * localDir.x + bitangent * localDir.y + normal * localDir.z;

        float4 hitResult = TraceRay(uv, depth, startView, rayDir, iRaySteps, thicknessLimit);

        totalIndirect += hitResult.rgb;
        totalOcclusion += hitResult.w;
    }

    totalIndirect /= (float)iRaysPerPixel;
    totalOcclusion /= (float)iRaysPerPixel;

    float ao = saturate(1.0 - totalOcclusion * aoIntensity);

    float fadeStart = fadeDistance * 0.8;
    float depthFade = saturate(1.0 - max(depth - fadeStart, 0.0) / max(fadeDistance - fadeStart, 0.001));
    totalIndirect *= depthFade;
    ao = lerp(1.0, ao, depthFade);

    outTex[tid.xy] = float4(totalIndirect, ao);
}
