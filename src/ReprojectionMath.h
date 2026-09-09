#pragma once
#include <cmath>
#include <cstring>
#include <limits>

// Column-major, matching HLSL. Following KenshiNative's precision fix, keep
// BOTH inversion and composition in double until the final GPU upload matrix.
namespace ReprojectionMath
{
inline bool Inverse(const float* input, double* output)
{
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(input[i])) return false;
        m[i] = input[i];
    }
    inv[0]  =  m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    double det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (!std::isfinite(det) || det == 0.0) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i)
    {
        inv[i] *= det;
        if (!std::isfinite(inv[i])) return false;
    }
    // Reject inverses whose cancellation/conditioning makes them unusable.
    // Test in double before narrowing anything to the GPU's float format.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            double value = 0;
            for (int k = 0; k < 4; ++k) value += m[k*4+r] * inv[c*4+k];
            if (!std::isfinite(value) || std::abs(value - (r == c ? 1.0 : 0.0)) > 1e-6)
                return false;
        }
    std::memcpy(output, inv, sizeof(inv));
    return true;
}

inline bool MultiplyInverse(const float* a, const float* b, float* output)
{
    double inverse[16];
    if (!Inverse(b, inverse)) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(a[i])) return false;
    float result[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            double value = 0;
            for (int k = 0; k < 4; ++k) value += double(a[k*4+r]) * inverse[c*4+k];
            if (!std::isfinite(value) || std::abs(value) > (std::numeric_limits<float>::max)())
                return false;
            result[c*4+r] = float(value);
        }
    std::memcpy(output, result, sizeof(result)); // failure leaves the caller's fallback intact
    return true;
}
}
