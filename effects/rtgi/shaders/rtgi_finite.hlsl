// isfinite() does not exist in vkd3d-shader, the HLSL compiler Wine/Proton
// uses, so test the exponent bits instead: all set means Inf or NaN. No 'u'
// literal suffix: vkd3d before 1.10 rejects it. Separate names, not overloads:
// vkd3d does not rank compatible overloads that differ only by vector width.
// Included once per shader, through rtgi_camera.hlsl or directly.
bool RtgiIsFinite(float v)
{
    return (asuint(v) & 0x7F800000) != 0x7F800000;
}

bool3 RtgiIsFinite3(float3 v)
{
    return (asuint(v) & 0x7F800000) != 0x7F800000;
}

bool4 RtgiIsFinite4(float4 v)
{
    return (asuint(v) & 0x7F800000) != 0x7F800000;
}
