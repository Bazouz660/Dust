// World-space test pattern for evaluating AA quality during camera motion.
// Reads depth, reconstructs world position via inverse view matrix, then draws
// a grid + shapes on the world XZ plane so the pattern moves with the camera.

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

    float4 invViewRow0;
    float4 invViewRow1;
    float4 invViewRow2;
    float4 invViewRow3;
};

Texture2D<float> depthTex : register(t0);
SamplerState     pointSamp : register(s0);

float3 uv_to_proj(float2 uv, float z)
{
    float2 uvtoprojADD = float2(-tanHalfFov * aspectRatio, -tanHalfFov);
    float2 uvtoprojMUL = -2.0 * uvtoprojADD;
    return float3((uv * uvtoprojMUL + uvtoprojADD) * z, z);
}

float GridLine(float coord, float lineWidth)
{
    float d = abs(frac(coord) - 0.5) - 0.5;
    float deriv = fwidth(coord);
    return 1.0 - saturate(abs(d) / max(deriv * lineWidth, 0.001));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float depth = depthTex.SampleLevel(pointSamp, uv, 0);

    if (depth < 1e-4)
        return float4(0.08, 0.08, 0.12, 1);

    float z = depth * reprojDepthScale + reprojDepthOffset;
    float3 viewPos = uv_to_proj(uv, z);

    float4 h = float4(viewPos, 1.0);
    float3 worldPos = float3(
        dot(invViewRow0, h),
        dot(invViewRow1, h),
        dot(invViewRow2, h)
    );

    // Scale: 100 world-units per grid cell
    float2 wp = worldPos.xz * 0.01;

    float gridFine  = max(GridLine(wp.x, 1.5), GridLine(wp.y, 1.5));
    float gridCoarse = max(GridLine(wp.x * 0.1, 1.5), GridLine(wp.y * 0.1, 1.5));

    // Diagonal crosshatch
    float2 rotated = float2(wp.x + wp.y, wp.x - wp.y) * 0.7071;
    float diag = max(GridLine(rotated.x, 1.5), GridLine(rotated.y, 1.5));

    // Circles at every 1000 world-unit intersection
    float2 cell = floor(wp * 0.1) * 10.0 + 5.0;
    float dist = length(wp - cell);
    float circDeriv = fwidth(dist);
    float circle = 1.0 - saturate(abs(dist - 3.0) / max(circDeriv * 1.5, 0.001));

    float3 bg = float3(0.15, 0.15, 0.2);
    float3 color = bg;
    color = lerp(color, float3(0.3, 0.3, 0.35), gridFine * 0.8);
    color = lerp(color, float3(0.5, 0.5, 0.6),  gridCoarse);
    color = lerp(color, float3(0.8, 0.6, 0.2),  diag * 0.6);
    color = lerp(color, float3(1.0, 1.0, 1.0),  circle);

    return float4(color, 1);
}
