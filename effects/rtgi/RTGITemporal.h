#pragma once
#include "../../src/ReprojectionMath.h"

namespace RTGITemporal
{
// Input is the game's raw inverse view (row-vector layout, RH). Output is
// row-vector LH, with positions measured in ray-distance / farClip units.
inline bool Reprojection(const float* current, const float* previous, float farClip, float* output)
{
    if (!std::isfinite(farClip) || farClip <= 0) return false;
    float previousRotation[16] = {};
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(current[i]) || !std::isfinite(previous[i])) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) previousRotation[r*4+c] = previous[r*4+c];
    previousRotation[15] = 1;
    double inverse[16];
    // Inverse() also works on row-major storage: inv(transpose(M)) = transpose(inv(M)).
    if (!ReprojectionMath::Inverse(previousRotation, inverse)) return false;
    float result[16] = {};
    const double sign[3] = {1, 1, -1};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            double value = 0;
            for (int k = 0; k < 3; ++k) value += double(current[r*4+k]) * inverse[k*4+c];
            result[r*4+c] = float(value * sign[r] * sign[c]);
        }
    // Subtract positions BEFORE rotation to avoid cancellation at world-scale coordinates.
    for (int c = 0; c < 3; ++c)
    {
        double value = 0;
        for (int k = 0; k < 3; ++k)
            value += (double(current[12+k]) - double(previous[12+k])) * inverse[k*4+c];
        result[12+c] = float(value * sign[c] / double(farClip));
    }
    result[15] = 1;
    for (float value : result) if (!std::isfinite(value)) return false;
    std::memcpy(output, result, sizeof(result));
    return true;
}
}
