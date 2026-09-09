#include "../src/ReprojectionMath.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

static void Camera(float* out, double position, double yaw)
{
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double view[16] = { c,0,s,0, 0,1,0,0, -s,0,c,0, -c*position,0,-s*position,1 };
    const double nearZ = .1, farZ = 2500;
    const double proj[16] = { 1.2,0,0,0, 0,1.8,0,0, 0,0,farZ/(farZ-nearZ),1, 0,0,-nearZ*farZ/(farZ-nearZ),0 };
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
        {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += proj[k*4+row] * view[col*4+k];
            out[col*4+row] = float(v);
        }
}

// Independent pivoted elimination oracle, not the production cofactor inverse.
static void ReferenceInverse(const float* input, double* inverse)
{
    double rows[4][8] = {};
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c) rows[r][c] = input[c*4+r];
        rows[r][4+r] = 1;
    }
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c+1; r < 4; ++r)
            if (std::abs(rows[r][c]) > std::abs(rows[pivot][c])) pivot = r;
        assert(rows[pivot][c] != 0);
        for (int k = 0; k < 8; ++k) std::swap(rows[pivot][k], rows[c][k]);
        const double divisor = rows[c][c];
        for (int k = 0; k < 8; ++k) rows[c][k] /= divisor;
        for (int r = 0; r < 4; ++r)
        {
            if (r == c) continue;
            const double factor = rows[r][c];
            for (int k = 0; k < 8; ++k) rows[r][k] -= factor * rows[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inverse[c*4+r] = rows[r][c+4];
}

int main()
{
    double worst = 0;
    for (double position : {0.0, 859.0, 4476.0, 20000.0})
        for (double yaw : {0.0, .005, .028, .7})
        {
            float current[16], previous[16], result[16];
            Camera(current, position, yaw);
            Camera(previous, position-.25, yaw-.005);
            assert(ReprojectionMath::MultiplyInverse(previous, current, result));
            double inv[16]; ReferenceInverse(current, inv);
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                {
                    double expected = 0;
                    for (int k = 0; k < 4; ++k) expected += double(previous[k*4+r]) * inv[c*4+k];
                    const double error = std::abs(result[c*4+r]-expected) / std::max(1.0, std::abs(expected));
                    worst = std::max(worst, error);
                    assert(error < 2e-6);
                }
            assert(ReprojectionMath::MultiplyInverse(current, current, result));
            for (int i = 0; i < 16; ++i) assert(std::abs(result[i] - (i%5 == 0 ? 1 : 0)) < 1e-6);
        }
    float identity[16] = {}, bad[16] = {}, output[16];
    for (int i = 0; i < 16; ++i) { identity[i] = i%5 == 0 ? 1.f : 0.f; output[i] = 123; }
    assert(!ReprojectionMath::MultiplyInverse(identity, bad, output));
    std::memcpy(bad, identity, sizeof(bad));
    bad[7] = std::numeric_limits<float>::quiet_NaN();
    assert(!ReprojectionMath::MultiplyInverse(identity, bad, output));
    bad[7] = std::numeric_limits<float>::infinity();
    assert(!ReprojectionMath::MultiplyInverse(identity, bad, output));
    assert(!ReprojectionMath::MultiplyInverse(bad, identity, output));
    for (float v : output) assert(v == 123); // failure never poisons the identity fallback
    // Small but well-conditioned matrices must not fail an arbitrary determinant cutoff.
    for (int i = 0; i < 16; ++i) bad[i] = identity[i] * 1e-10f;
    assert(ReprojectionMath::MultiplyInverse(bad, bad, output));
    for (int i = 0; i < 16; ++i) assert(std::abs(output[i]-identity[i]) < 1e-6);
    std::printf("reprojection oracle: 16 camera pairs, worst relative error %.3g\n", worst);
}
