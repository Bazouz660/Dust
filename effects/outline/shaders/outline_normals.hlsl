// isfinite() does not exist in vkd3d-shader, the HLSL compiler Wine/Proton
// uses, so test the exponent bits instead: all set means Inf or NaN. No 'u'
// literal suffix: vkd3d before 1.10 rejects it.
bool3 OutlineIsFinite(float3 v)
{
    return (asuint(v) & 0x7F800000) != 0x7F800000;
}

// Some game materials write non-unit normals into the GBuffer. Compare
// directions, not lengths, or a flat surface can become a solid outline.
float OutlineNormalDifference(float3 a, float3 b)
{
    float aa = dot(a, a);
    float bb = dot(b, b);
    // Includes the quantized neutral normal. Let depth detect these edges.
    if (!all(OutlineIsFinite(a)) || !all(OutlineIsFinite(b)) || aa <= 1e-4 || bb <= 1e-4)
        return 0.0;
    float3 difference = a * rsqrt(aa) - b * rsqrt(bb);
    // Equivalent to 1-dot for unit vectors, exactly zero for identical input.
    return 0.5 * dot(difference, difference);
}
