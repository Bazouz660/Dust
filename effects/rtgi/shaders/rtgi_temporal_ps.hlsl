// RTGI Temporal Accumulation Pass
//
// No-reprojection history read (always at current UV) to avoid buffer contamination
// that causes ghosting "pop" when camera stops. Instead, uses per-pixel motion vectors
// from reprojection to scale alpha — pixels that moved get flushed, static pixels
// accumulate. When camera stops, all motion = 0 → instant convergence, no delay.
//
// Output: RGB = accumulated indirect light, A = accumulated AO

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
    row_major float4x4 reprojMatrix; // currentInvView * prevView
    float    motionMagnitude;
    float    _pad0;
    float    _pad1;
    float    _pad2;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    float4 current = currentGI.SampleLevel(pointClamp, uv, 0);

    // ---- First frame or sky ----
    if (frameIndex < 1.0 || depth <= 0.0001)
        return current;

    // ---- Always read history at current UV (no reprojection contamination) ----
    float4 history = historyGI.SampleLevel(pointClamp, uv, 0);

    // ---- Per-pixel motion vector from reprojection ----
    // Used ONLY to determine alpha, NOT to read history.
    float pixelMotion = 0.0;

    float3 viewPos = ReconstructViewPos(uv, depth, tanHalfFov, aspectRatio);
    float3 prevViewPos = mul(float4(viewPos, 1.0), reprojMatrix).xyz;

    if (prevViewPos.z > 0.0001)
    {
        float2 prevUV = ViewPosToUV(prevViewPos, tanHalfFov, aspectRatio);
        float2 motionVec = abs((prevUV - uv) * viewportSize); // in pixels
        pixelMotion = max(motionVec.x, motionVec.y); // Chebyshev distance — no sqrt
    }
    else
    {
        pixelMotion = 100.0;
    }

    // ---- Depth-change detection (catches disocclusion) ----
    float prevDepth = prevDepthTex.SampleLevel(pointClamp, uv, 0);
    float depthChange = abs(depth - prevDepth) / max(depth, 0.001);
    static const float DEPTH_SIGMA_SQ2 = 2.0 * 0.008 * 0.008;
    float depthStability = exp(-depthChange * depthChange / DEPTH_SIGMA_SQ2);

    // ---- Adaptive alpha ----
    float baseAlpha = max(1.0 - temporalBlend, 0.08);

    // Motion-based alpha: gradual ramp
    float motionAlpha = saturate(pixelMotion * 0.2);

    // Depth-based alpha: large depth change → flush
    float depthAlpha = 1.0 - depthStability;

    // Combined: take the stronger signal
    float dynamicAlpha = max(motionAlpha, depthAlpha);

    // Cap motion alpha — even at max motion, keep some history for noise reduction
    float alpha = lerp(baseAlpha, 0.8, dynamicAlpha);
    float aoAlpha = lerp(max(baseAlpha, 0.08), 0.9, dynamicAlpha);

    float4 result;
    result.rgb = lerp(history.rgb, current.rgb, alpha);
    result.a   = lerp(history.a,   current.a,   aoAlpha);
    return result;
}
