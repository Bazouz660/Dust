// These are snapshots of the game's deferred CB, not CPU readbacks.
cbuffer CurrentCamera : register(b1)
{
    float4 currentFog : packoffset(c2); // x = ray-distance normalization (far clip)
    row_major float4x4 currentInverse : packoffset(c8);
};
cbuffer PreviousCamera : register(b2)
{
    float4 previousFog : packoffset(c2);
    row_major float4x4 previousInverse : packoffset(c8);
};
cbuffer CameraHistoryParams : register(b3)
{
    row_major float4x4 fallbackReprojection;
    float fallbackFar;
    float fallbackPreviousFar;
    float useGpuCamera;
    float cameraHistoryValid;
};

float3 RTGIWorldNormalToView(float3 n, float3 right, float3 up, float3 forward)
{
    if (useGpuCamera > 0.5) {
        right = float3(currentInverse[0][0], currentInverse[1][0], currentInverse[2][0]);
        up = float3(currentInverse[0][1], currentInverse[1][1], currentInverse[2][1]);
        forward = float3(currentInverse[0][2], currentInverse[1][2], currentInverse[2][2]);
    }
    return float3(dot(n, right), dot(n, up), -dot(n, forward));
}

bool RTGIPreviousPosition(float2 uv, float depth, float thf, float ar,
                          out float2 previousUV, out float previousDepth)
{
    previousUV = uv; previousDepth = depth;
    if (cameraHistoryValid < 0.5) return false;
    float3 ray = normalize(float3((uv.x * 2 - 1) * ar * thf, (1 - uv.y * 2) * thf, 1));
    float3 previous;
    if (useGpuCamera > 0.5) {
        if (currentFog.x <= 0 || previousFog.x <= 0) return false;
        float3 rh = ray * float3(1, 1, -1) * depth * currentFog.x;
        // Camera-relative subtraction avoids adding/subtracting large world positions.
        float3 delta = mul(rh, (float3x3)currentInverse)
                     + (currentInverse[3].xyz - previousInverse[3].xyz);
        float3 a = previousInverse[0].xyz, b = previousInverse[1].xyz, c = previousInverse[2].xyz;
        float det = dot(a, cross(b, c));
        if (abs(det) < 1e-6) return false;
        previous = float3(dot(delta, cross(b, c)), dot(delta, cross(c, a)), dot(delta, cross(a, b))) / det;
        previous *= float3(1, 1, -1) / previousFog.x;
    } else {
        previous = mul(float4(ray * depth, 1), fallbackReprojection).xyz;
    }
    if (!all(isfinite(previous)) || previous.z <= 0.00001) return false;
    previousUV = float2(previous.x / (previous.z * ar * thf) * .5 + .5,
                       .5 - previous.y / (previous.z * thf) * .5);
    previousDepth = length(previous);
    return all(previousUV >= 0) && all(previousUV <= 1);
}

bool RTGIHistoryDepthMatches(float expected, float sampled)
{
    return isfinite(sampled) && sampled > 0.0001
        && abs(expected - sampled) <= max(expected * 0.02, 1e-5);
}
