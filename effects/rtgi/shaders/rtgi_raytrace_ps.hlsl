// RTGI Ray Trace Pass
// Casts screen-space rays, marches depth buffer, samples lit scene radiance at hits.
// Uses Reinhard normalization on hit color to preserve hue at high brightness.
//
// Output: RGBA16F — RGB = indirect light (raw), A = occlusion

static const float PI = 3.14159265358979;
static const float TWO_PI = 6.28318530717959;

// ---- Noise ----

// Interleaved Gradient Noise (Jimenez). Blue-noise-like spatial distribution,
// so per-frame error averages cleanly under temporal accumulation + denoise.
float IGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
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

// ---- Hemisphere sampling ----

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

// ---- Color utilities ----

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- Resources ----

Texture2D<float>  depthTex    : register(t0);
Texture2D<float4> sceneTex    : register(t1); // Lit HDR scene
Texture2D<float4> prevGI      : register(t2); // Previous frame GI (multi-bounce)
Texture2D<float4> normalsTex  : register(t3); // GBuffer view-space normals
SamplerState      pointClamp  : register(s0);
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
    float  thicknessCurve;
    float  _pad0;
    float2 sampleJitter; // render-viewport UV pixels, in [-0.5, 0.5]
    float2 _padJitter;
    float4 camRight;   // camera right axis in world space
    float4 camUp;      // camera up axis in world space
    float4 camForward; // camera forward axis in world space
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

        if (depthDiff > 0.0 && depthDiff < thicknessLimit)
        {
            float hitT = t;

            if (numSteps >= 8)
            {
                float tLo = (float)(i - 1) * stepSize;
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
            float3 hitColor = sceneColor / (1.0 + lum);

            if (bounceIntensity > 0.0)
            {
                float3 prevBounce = prevGI.SampleLevel(linearClamp, hitUV, 0).rgb;
                float prevLum = Luminance(prevBounce);
                prevBounce = prevBounce / (1.0 + prevLum);
                hitColor += prevBounce * bounceIntensity;
            }

            float attenuation = saturate(1.0 - hitT * hitT * hitT);
            float2 edgeDist = min(hitUV, 1.0 - hitUV);
            float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 10.0);
            float occlusion = (1.0 - hitT * hitT);

            return float4(hitColor * attenuation * edgeFade, occlusion);
        }
    }

    return float4(0, 0, 0, 0);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // Sub-pixel temporal jitter: each frame, the render-res pixel samples
    // a different sub-pixel position in the full-res depth buffer. Over the
    // 16-frame Halton cycle, quarter-res covers its full 4×4 footprint and
    // half-res covers its 2×2 footprint. Temporal accumulation averages the
    // samples, recovering detail that would otherwise be lost at reduced res.
    uv += sampleJitter * invViewportSize;

    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (depth <= 0.0001 || depth > fadeDistance)
        return float4(0, 0, 0, 1);

    float3 worldN = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float3 normal;
    normal.x =  dot(worldN, camRight.xyz);
    normal.y =  dot(worldN, camUp.xyz);
    normal.z = -dot(worldN, camForward.xyz);
    normal = normalize(normal);

    float3 startView = ReconstructViewPos(uv, depth, tanHalfFov, aspectRatio);
    float3 tangent, bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);

    int iRaysPerPixel = max(1, (int)raysPerPixel);
    int iRaySteps = max(1, (int)raySteps);

    // Thickness curve: exponent < 1 tightens the "back wall" for distant
    // geometry. Hoisted out of the ray loop — constant per pixel.
    float thicknessLimit = thickness * pow(depth, thicknessCurve);

    // Per-frame jitter of the IGN input breaks its directional banding — stripes
    // shift each frame so temporal accumulation averages them out.
    float2 jitter = float2(frac(frameIndex * 0.7548776662), frac(frameIndex * 0.5698402910)) * 127.0;
    float2 blueNoise = float2(IGN(pos.xy + jitter), IGN(pos.xy + jitter + float2(23.97, 47.31)));
    float temporalPhase = frac(frameIndex * 0.618033988);

    float3 totalIndirect = float3(0, 0, 0);
    float  totalOcclusion = 0.0;

    [loop]
    for (int ray = 0; ray < iRaysPerPixel; ray++)
    {
        // Cranley-Patterson rotation: blue-noise offset + per-ray + temporal phase
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

    // Depth fade
    float fadeStart = fadeDistance * 0.8;
    float depthFade = saturate(1.0 - max(depth - fadeStart, 0.0) / max(fadeDistance - fadeStart, 0.001));
    totalIndirect *= depthFade;
    ao = lerp(1.0, ao, depthFade);

    return float4(totalIndirect, ao);
}
