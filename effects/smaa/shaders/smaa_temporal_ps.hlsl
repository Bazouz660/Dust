// SMAA Temporal Resolve — stabilizes SMAA 1x output by blending with a
// camera-reprojected, neighborhood-clipped history. Goal: kill edge crawl /
// aliasing flicker under camera motion while staying sharp (no ghosting/blur).
//
// Conventions match the proven RTGI/SSAO reprojection pipeline:
//   - view-space: X right, Y up, Z into scene
//   - linear depth: z = depth * farPlane + 1.0
//   - reproj = currentInvView * prevView, used as mul(float4(viewPos,1), reproj)
//
// Sharpness comes from Catmull-Rom history sampling (counters bilinear blur).
// Anti-ghosting comes from YCoCg neighborhood variance clipping + residual-
// velocity-adaptive feedback. A small dead-zone absorbs the engine's per-frame
// view-matrix FP jitter so a still camera converges fully.

cbuffer SMAACB : register(b0)
{
    float  invWidth, invHeight, width, height;   // == rtMetrics (shared with other passes)
    float  lumaThreshold, depthThreshold;
    int    edgeMode;
    int    temporalEnabled;

    float  tanHalfFov;
    float  aspectRatio;
    float  temporalStrength;
    float  farPlane;

    row_major float4x4 reproj;                    // currentInvView * prevView

    float  frameIndex;
    float  hasHistory;
    float  _pad0, _pad1;
};

Texture2D<float4> currentTex : register(t0);   // SMAA 1x output, this frame
Texture2D<float4> historyTex : register(t1);   // resolved output, previous frame
Texture2D<float>  depthTex   : register(t2);
SamplerState      pointSamp  : register(s0);
SamplerState      linearSamp : register(s1);   // linear CLAMP

// ---- View-space reconstruction ----
// OGRE view space is right-handed with Z *behind* the camera, so in-front
// points have negative Z (matches the game's frustum-corner reconstruction
// where the far corner has z = -farClip). The raw inverseView matrix expects
// this convention, so the reconstructed Z must be negative.
float3 ReconstructViewPos(float2 uv, float z)   // z = linear distance, positive
{
    float3 p;
    p.x = (uv.x * 2.0 - 1.0) * aspectRatio * tanHalfFov * z;
    p.y = (1.0 - uv.y * 2.0) * tanHalfFov * z;
    p.z = -z;                                   // OGRE RH: forward is -Z
    return p;
}

float2 ViewPosToUV(float3 vp)
{
    float d = -vp.z;                            // positive in-front distance
    float2 uv;
    uv.x = (vp.x / (d * aspectRatio * tanHalfFov)) * 0.5 + 0.5;
    uv.y = 0.5 - (vp.y / (d * tanHalfFov)) * 0.5;
    return uv;
}

// ---- YCoCg (variance clipping is more stable in this space) ----
float3 RGB_to_YCoCg(float3 c)
{
    return float3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                   0.5  * c.r              - 0.5  * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b );
}
float3 YCoCg_to_RGB(float3 c)
{
    return float3( c.x + c.y - c.z,
                   c.x       + c.z,
                   c.x - c.y - c.z );
}

float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 current, float3 history)
{
    float3 center = 0.5 * (aabbMax + aabbMin);
    float3 extent = 0.5 * (aabbMax - aabbMin) + 1e-5;
    float3 offset = history - center;
    float3 ts = abs(extent / max(abs(offset), 1e-5));
    float t = saturate(min(ts.x, min(ts.y, ts.z)));
    return center + offset * t;
}

// ---- Catmull-Rom history fetch (5 bilinear taps). Sharp resampling that
// avoids the per-frame softening of plain bilinear history reads. ----
float3 SampleHistoryCatmullRom(float2 uv)
{
    float2 texSize = float2(width, height);
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 invTex = float2(invWidth, invHeight);
    float2 texPos0  = (texPos1 - 1.0)      * invTex;
    float2 texPos3  = (texPos1 + 2.0)      * invTex;
    float2 texPos12 = (texPos1 + offset12) * invTex;

    float3 r = 0;
    r += historyTex.SampleLevel(linearSamp, float2(texPos12.x, texPos0.y),  0).rgb * (w12.x * w0.y);
    r += historyTex.SampleLevel(linearSamp, float2(texPos0.x,  texPos12.y), 0).rgb * (w0.x  * w12.y);
    r += historyTex.SampleLevel(linearSamp, float2(texPos12.x, texPos12.y), 0).rgb * (w12.x * w12.y);
    r += historyTex.SampleLevel(linearSamp, float2(texPos3.x,  texPos12.y), 0).rgb * (w3.x  * w12.y);
    r += historyTex.SampleLevel(linearSamp, float2(texPos12.x, texPos3.y),  0).rgb * (w12.x * w3.y);
    return max(r, 0.0); // negative lobes can undershoot; LDR clamp
}

struct PSOut
{
    float4 display : SV_Target0;
    float4 history : SV_Target1;
};

PSOut main(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
    PSOut o;
    float4 current = currentTex.SampleLevel(pointSamp, uv, 0);

    float depth = depthTex.SampleLevel(pointSamp, uv, 0);

    // First frame, temporal off, or sky → pass through.
    if (temporalEnabled == 0 || hasHistory < 0.5 || depth < 1e-4)
    {
        o.display = current;
        o.history = current;
        return o;
    }

    // Reproject current pixel into the previous frame. Depth is linear
    // viewZ/farClip, so the world-unit view distance is depth*farClip — this
    // MUST use the true far clip (~50000) or the parallax is mis-scaled and
    // history misaligns under camera translation.
    float z = depth * farPlane;
    float3 viewPos = ReconstructViewPos(uv, z);
    float3 prevView = mul(float4(viewPos, 1.0), reproj).xyz;

    // In-front points have negative Z in OGRE view space; reject anything that
    // landed behind the previous camera.
    if (prevView.z >= -1e-4)
    {
        o.display = current;
        o.history = current;
        return o;
    }

    float2 prevUV = ViewPosToUV(prevView);
    if (any(prevUV < 0.0) || any(prevUV > 1.0))
    {
        o.display = current;
        o.history = current;
        return o;
    }

    float3 history = SampleHistoryCatmullRom(prevUV);

    // Neighborhood variance clip (YCoCg) → clamps history to the local color
    // range so it can't smear across edges (anti-ghosting + keeps edges crisp).
    float2 px = float2(invWidth, invHeight);
    float3 m1 = 0, m2 = 0;
    float3 nbMin = 1e5, nbMax = -1e5;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        float3 s = RGB_to_YCoCg(currentTex.SampleLevel(pointSamp, uv + float2(x, y) * px, 0).rgb);
        m1 += s; m2 += s * s;
        nbMin = min(nbMin, s);
        nbMax = max(nbMax, s);
    }
    m1 /= 9.0; m2 /= 9.0;
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    float3 clipMin = max(nbMin, m1 - 1.25 * sigma);
    float3 clipMax = min(nbMax, m1 + 1.25 * sigma);

    float3 currYCC = RGB_to_YCoCg(current.rgb);
    float3 histYCC = RGB_to_YCoCg(history);
    float3 clipped = YCoCg_to_RGB(ClipAABB(clipMin, clipMax, currYCC, histYCC));

    // Residual (post-reprojection) screen velocity. Static geometry under
    // camera motion has ~0 residual → full stabilization. Moving objects have
    // large residual → flush history (no trails). Dead-zone absorbs the
    // engine's per-frame view-matrix FP jitter (~1px) so a still camera locks.
    float2 vel = (prevUV - uv) * float2(width, height);
    float speed = max(length(vel) - 1.0, 0.0);
    float feedback = temporalStrength * saturate(1.0 - speed * 0.05);

    float3 result = lerp(current.rgb, clipped, feedback);

    o.display = float4(result, current.a);
    o.history = float4(result, 1.0);
    return o;
}
