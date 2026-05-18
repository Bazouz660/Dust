// Temporal accumulation -- Kalman filter with variance-based neighborhood
// clamping. Runs BEFORE spatial filtering. Reads raw reinterleaved AO (t0),
// history (t1), raw depth (t2).
//
// History format: .x=filtered AO, .y=signed_depth, .z=beta, .w=covariance.
// Filter passes read .x (GatherRed=AO) and .y (GatherGreen=depth) -- preserved.

#include "ao_pipeline.hlsli"

Texture2D<float4> CurrAO       : register(t0);
Texture2D<float4> HistoryPrev  : register(t1);
Texture2D<float>  RawDepth     : register(t2);
SamplerState      PointSamp    : register(s0);
SamplerState      LinearSamp   : register(s1);

struct VSOUT
{
    float4 vpos : SV_Position;
    float2 uv   : TEXCOORD0;
};

float4 main(VSOUT i) : SV_Target
{
    float4 curr        = CurrAO.SampleLevel(PointSamp, i.uv, 0);
    float curr_ao      = curr.x;
    float signed_depth = curr.y;

    float centerdepth  = RawDepth.SampleLevel(PointSamp, i.uv, 0);

    float z = max(centerdepth * ReprojDepthScale + ReprojDepthOffset, 0.01);
    float3 p_curr = uv_to_proj(i.uv, z);
    float3 p_prev = mul(PrevFromCurrView, float4(p_curr, 1.0)).xyz;
    float2 prev_uv = proj_to_uv(p_prev);

    float4 prev_data = HistoryPrev.SampleLevel(LinearSamp, prev_uv, 0);
    float prev_depth = abs(prev_data.y);
    float old_beta   = prev_data.z;
    float old_cov    = prev_data.w;

    bool valid = all(prev_uv >= 0.0) && all(prev_uv <= 1.0) && TemporalEnabled != 0;

    float depth_err = abs(prev_depth - centerdepth) / max(centerdepth, 1e-5);
    if (depth_err > 0.1)
        valid = false;

    // 5x5 neighborhood statistics for variance-based clamping.
    float2 moments = 0;
    [unroll] for (int dy = -2; dy <= 2; dy++)
    [unroll] for (int dx = -2; dx <= 2; dx++)
    {
        float tap = CurrAO.SampleLevel(PointSamp, i.uv + float2(dx, dy) * BufferPixelSize, 0).x;
        moments += float2(tap, tap * tap);
    }
    moments /= 25.0;
    float curr_sigma = sqrt(abs(moments.y - moments.x * moments.x));
    float curr_mean  = moments.x;

    // Kalman filter (scalar, lambda=forgetting factor).
    float lambda = 0.9;

    if (!valid || abs(old_cov) < 1e-7)
    {
        old_beta = curr_ao;
        old_cov  = 10;
    }

    float predicted_value = old_beta;
    float deviations_from_target = abs(predicted_value - curr_mean) / max(1e-7, curr_sigma);
    float clamped = clamp(predicted_value, curr_mean - curr_sigma, curr_mean + curr_sigma);

    [branch]
    if (predicted_value != clamped)
    {
        float clamp_strength = (deviations_from_target - 1) / deviations_from_target;
        predicted_value = old_beta = clamped;
        old_cov = lerp(old_cov, 3.0, clamp_strength);
    }

    float error    = curr_ao - predicted_value;
    float Q_t      = old_cov / (lambda + old_cov);
    float new_beta = old_beta + Q_t * error;
    float new_cov  = (old_cov - Q_t * old_cov) / lambda;

    float result = new_beta;

    return float4(result, signed_depth, new_beta, new_cov);
}
