// RTGI Common Utilities
// Shared constants, sampling, reconstruction, and noise functions.

#ifndef RTGI_COMMON_HLSL
#define RTGI_COMMON_HLSL

static const float PI = 3.14159265358979;
static const float TWO_PI = 6.28318530717959;
static const float HALF_PI = 1.57079632679490;
static const float INV_PI = 0.31830988618379;

// ============================================================
// Noise functions
// ============================================================

// Interleaved Gradient Noise — good spatial distribution, cheap
float InterleavedGradientNoise(float2 screenPos)
{
    return frac(52.9829189 * frac(0.06711056 * screenPos.x + 0.00583715 * screenPos.y));
}

// Hash-based noise for temporal variation
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

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

// ============================================================
// View-space reconstruction
// ============================================================

float3 ReconstructViewPos(float2 uv, float depth, float tanHalfFov, float aspectRatio)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * aspectRatio * tanHalfFov * depth;
    pos.y = (1.0 - uv.y * 2.0) * tanHalfFov * depth;
    pos.z = depth;
    return pos;
}

// Project a view-space position back to UV coordinates
float2 ViewPosToUV(float3 viewPos, float tanHalfFov, float aspectRatio)
{
    float2 uv;
    uv.x = (viewPos.x / (viewPos.z * aspectRatio * tanHalfFov)) * 0.5 + 0.5;
    uv.y = 0.5 - (viewPos.y / (viewPos.z * tanHalfFov)) * 0.5;
    return uv;
}

// ============================================================
// Normal reconstruction from depth (smallest-delta method)
// ============================================================

float3 ReconstructNormal(Texture2D<float> depthTex, SamplerState samp,
                         float2 uv, float depth, float2 invViewport,
                         float tanHalfFov, float aspectRatio)
{
    float depthR = depthTex.SampleLevel(samp, uv + float2( invViewport.x, 0), 0);
    float depthL = depthTex.SampleLevel(samp, uv + float2(-invViewport.x, 0), 0);
    float depthU = depthTex.SampleLevel(samp, uv + float2(0, -invViewport.y), 0);
    float depthD = depthTex.SampleLevel(samp, uv + float2(0,  invViewport.y), 0);

    float3 viewPos = ReconstructViewPos(uv, depth, tanHalfFov, aspectRatio);

    float3 ddxPos, ddyPos;
    if (abs(depthR - depth) < abs(depthL - depth))
        ddxPos = ReconstructViewPos(uv + float2(invViewport.x, 0), depthR, tanHalfFov, aspectRatio) - viewPos;
    else
        ddxPos = viewPos - ReconstructViewPos(uv - float2(invViewport.x, 0), depthL, tanHalfFov, aspectRatio);

    if (abs(depthD - depth) < abs(depthU - depth))
        ddyPos = ReconstructViewPos(uv + float2(0, invViewport.y), depthD, tanHalfFov, aspectRatio) - viewPos;
    else
        ddyPos = viewPos - ReconstructViewPos(uv - float2(0, invViewport.y), depthU, tanHalfFov, aspectRatio);

    return normalize(cross(ddxPos, ddyPos));
}

// ============================================================
// Cosine-weighted hemisphere sampling
// ============================================================

// Generate a cosine-weighted direction on the hemisphere around (0,0,1)
float3 CosineHemisphereSample(float2 xi)
{
    float r = sqrt(xi.x);
    float phi = TWO_PI * xi.y;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - xi.x));
    return float3(x, y, z);
}

// Build an orthonormal basis from a normal vector (Frisvad's method, revised)
void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    if (n.z < -0.9999999)
    {
        t = float3(0, -1, 0);
        b = float3(-1, 0, 0);
        return;
    }
    float a = 1.0 / (1.0 + n.z);
    float d = -n.x * n.y * a;
    t = float3(1.0 - n.x * n.x * a, d, -n.x);
    b = float3(d, 1.0 - n.y * n.y * a, -n.y);
}

// Transform a hemisphere sample direction to world/view-space given a normal
float3 HemisphereToNormal(float3 sampleDir, float3 normal)
{
    float3 t, b;
    BuildOrthonormalBasis(normal, t, b);
    return normalize(t * sampleDir.x + b * sampleDir.y + normal * sampleDir.z);
}

// ============================================================
// Matrix helpers for temporal reprojection
// ============================================================

// Transform view-space position to world-space using inverseView (row-major in CB).
// HLSL default column-major interpretation of row-major data means
// mul(v, M) does: result[j] = dot(v, ogre_row_j)
float3 ViewToWorld(float3 viewPos, float4x4 invView)
{
    return mul(float4(viewPos, 1.0), invView).xyz;
}

// Transform world-space position to view-space using inverseView of another frame.
// We need to invert the transform: viewPos = rotation^T * (worldPos - translation)
float3 WorldToView(float3 worldPos, float4x4 invView)
{
    // Extract camera position (translation) from the last row of row-major inverseView
    // In our CB layout (row-major stored, HLSL column-major interpretation):
    // invView[3] = last OGRE row = (Tx, Ty, Tz, 1)
    float3 camPos = float3(invView[3][0], invView[3][1], invView[3][2]);
    float3 rel = worldPos - camPos;

    // Rotation columns of inverseView (= camera axes in world space)
    // In row-major interpretation: column i = (invView[0][i], invView[1][i], invView[2][i])
    // With OGRE row-major data in HLSL column-major: invView[i] is OGRE row i
    // So OGRE column i = (invView[0][i], invView[1][i], invView[2][i])
    float3 viewPos;
    viewPos.x = dot(rel, float3(invView[0][0], invView[1][0], invView[2][0]));
    viewPos.y = dot(rel, float3(invView[0][1], invView[1][1], invView[2][1]));
    viewPos.z = dot(rel, float3(invView[0][2], invView[1][2], invView[2][2]));
    return viewPos;
}

// ============================================================
// Luminance and color utilities
// ============================================================

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 AdjustSaturation(float3 color, float saturation)
{
    float lum = Luminance(color);
    return lerp(float3(lum, lum, lum), color, saturation);
}

#endif // RTGI_COMMON_HLSL
