// SMAA Temporal Resolve — blends current SMAA output with reprojected history.
// View-space convention: X right, Y down, Z into scene.
// depth_to_z = depth * FarPlane + 1.0 (Kenshi linear depth, proven in SSAO).

cbuffer SMAACB : register(b0)
{
    float  invWidth, invHeight, width, height;
    float  lumaThreshold, depthThreshold;
    int    edgeMode;
    int    temporalEnabled;

    float  tanHalfFov;
    float  aspectRatio;
    float  temporalStrength;
    float  reprojDepthScale;

    float  reprojDepthOffset;
    int    debugMotionVectors;
    float  jitterX, jitterY;

    float4 reprojRow0;
    float4 reprojRow1;
    float4 reprojRow2;
    float4 reprojRow3;
};

Texture2D<float4> currentTex : register(t0);
Texture2D<float4> historyTex : register(t1);
Texture2D<float>  depthTex   : register(t2);
SamplerState      pointSamp  : register(s0);
SamplerState      linearSamp : register(s1);

// View-space projection helper: UV + depth → view position
float3 uv_to_proj(float2 uv, float z)
{
    float2 uvtoprojADD = float2(-tanHalfFov * aspectRatio, -tanHalfFov);
    float2 uvtoprojMUL = -2.0 * uvtoprojADD;
    return float3((uv * uvtoprojMUL + uvtoprojADD) * z, z);
}

// Inverse projection helper: view position → UV
float2 proj_to_uv(float3 pos)
{
    if (pos.z <= 0.001) return float2(-1.0, -1.0);
    float2 uvtoprojADD = float2(-tanHalfFov * aspectRatio, -tanHalfFov);
    float2 uvtoprojMUL = -2.0 * uvtoprojADD;
    return ((pos.xy / pos.z) - uvtoprojADD) / uvtoprojMUL;
}

float3 RGB_to_YCoCg(float3 c)
{
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r              - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b
    );
}

float3 YCoCg_to_RGB(float3 c)
{
    return float3(
        c.x + c.y - c.z,
        c.x       + c.z,
        c.x - c.y - c.z
    );
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

struct PSOut
{
    float4 display : SV_Target0;
    float4 history : SV_Target1;
};

PSOut main(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
    PSOut o;
    float4 current = currentTex.SampleLevel(pointSamp, uv, 0);

    // Depth → z: linear depth conversion (sky=0 maps to far)
    float depth = depthTex.SampleLevel(pointSamp, uv, 0);
    float farZ = reprojDepthScale + reprojDepthOffset;
    float z = (depth < 1e-4) ? farZ : depth * reprojDepthScale + reprojDepthOffset;

    // Current pixel in view-space (use unjittered UV for reprojection)
    float3 p_curr = uv_to_proj(uv, z);

    // Reproject: prevView * currInvView (row-major)
    float4 h = float4(p_curr, 1.0);
    float3 p_prev = float3(
        dot(reprojRow0, h),
        dot(reprojRow1, h),
        dot(reprojRow2, h)
    );

    float2 prev_uv = proj_to_uv(p_prev);

    // Debug: motion vector visualization
    if (debugMotionVectors)
    {
        float2 motion_px = (prev_uv - uv) * float2(width, height);
        float speed = length(motion_px);
        float2 vis = motion_px * 0.002 + 0.5;
        o.display = float4(saturate(vis.x), saturate(vis.y), saturate(speed * 0.002), 1);
        o.history = current;
        return o;
    }

    // Out-of-bounds: no history available
    if (any(prev_uv < 0.0) || any(prev_uv > 1.0))
    {
        o.display = current;
        o.history = current;
        return o;
    }

    float4 history = historyTex.SampleLevel(linearSamp, prev_uv, 0);

    // Neighborhood clipping in YCoCg (sample around de-jittered position)
    float2 pxSize = float2(invWidth, invHeight);
    float3 m1 = 0, m2 = 0;
    float3 nbMin = 1e5, nbMax = -1e5;

    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        float3 s = currentTex.SampleLevel(pointSamp, uv + float2(x, y) * pxSize, 0).rgb;
        float3 ycc = RGB_to_YCoCg(s);
        m1 += ycc;
        m2 += ycc * ycc;
        nbMin = min(nbMin, ycc);
        nbMax = max(nbMax, ycc);
    }

    m1 /= 9.0;
    m2 /= 9.0;
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    float3 varMin = m1 - 1.25 * sigma;
    float3 varMax = m1 + 1.25 * sigma;

    float3 clipMin = max(nbMin, varMin);
    float3 clipMax = min(nbMax, varMax);

    float3 currYCC = RGB_to_YCoCg(current.rgb);
    float3 histYCC = RGB_to_YCoCg(history.rgb);
    float3 clipped = ClipAABB(clipMin, clipMax, currYCC, histYCC);
    history.rgb = YCoCg_to_RGB(clipped);

    // Velocity-adaptive blend
    float2 velocity = prev_uv - uv;
    float speed = length(velocity * float2(width, height));
    float blend = temporalStrength * saturate(1.0 - speed * 0.1);

    float4 result = lerp(current, history, blend);

    o.display = result;
    o.history = result;
    return o;
}
