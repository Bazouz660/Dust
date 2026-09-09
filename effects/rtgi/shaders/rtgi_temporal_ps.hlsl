// Reproject history to the previous surface, reject disocclusions, and clip
// stale radiance/AO to the current neighborhood before residual smoothing.
#include "rtgi_camera.hlsl"

// ---- Resources ----

Texture2D<float4> currentGI    : register(t0);
Texture2D<float4> historyGI    : register(t1);
Texture2D<float>  depthTex     : register(t2);
Texture2D<float>  prevDepthTex : register(t3);
SamplerState      pointClamp   : register(s0);
SamplerState      linearClamp  : register(s1);

cbuffer TemporalParams : register(b0)
{
    float2   viewportSize;
    float2   invViewportSize;
    float    tanHalfFov;
    float    aspectRatio;
    float    temporalBlend;
    float    frameIndex;
    row_major float4x4 reprojMatrix; // LH current-to-previous view, translation divided by farClip
    float    _reservedMotion;
    float    _pad0;
    float    _pad1;
    float    _pad2;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    int2 pix = int2(pos.xy);
    float depth = depthTex.SampleLevel(pointClamp, uv, 0);
    float4 current = currentGI.Load(int3(pix, 0));
    if (frameIndex < 1.0 || temporalBlend <= 0.0 || depth <= 0.0001)
        return current;

    float2 previousUV; float expectedDepth;
    if (!RTGIPreviousPosition(uv, depth, tanHalfFov, aspectRatio, previousUV, expectedDepth)
        || !RTGIHistoryDepthMatches(expectedDepth, prevDepthTex.SampleLevel(pointClamp, previousUV, 0)))
        return current;

    float4 history = historyGI.SampleLevel(linearClamp, previousUV, 0);
    if (!all(isfinite(history))) return current;

    // Clamp HISTORY, not the current frame. Include the center and clamp loads
    // at image boundaries: first-frame and accumulated output share one signal.
    uint width, height; currentGI.GetDimensions(width, height);
    int2 last = int2(width, height) - 1;
    float4 lower = current, upper = current;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float4 sample = currentGI.Load(int3(clamp(pix + int2(x,y), int2(0,0), last), 0));
        lower = min(lower, sample); upper = max(upper, sample);
    }
    history = clamp(history, lower, upper);
    return lerp(current, history, saturate(temporalBlend));
}
