#pragma once

#include <regex>
#include <string>

// Kenshi fits the RTWSM shadow camera's depth range tightly to the view frustum
// (Kenshi_x64+0x865640: light-space AABB of the 8 frustum corners, no margin) and its
// caster shaders clamp z to the near plane so casters sunward of the frustum still
// rasterise:
//     outPos.z = max(outPos.z, 0.0); // avoid clipping
//     depth = outPos.z;
// The clamped value is also what lands in the R32F shadow map, so every such caster is
// stored AT the near plane. That plane moves with the camera (it passes through the
// frustum point nearest the sun, often the camera itself when zoomed in), so PCSS
// measured receiver-to-near-plane instead of receiver-to-blocker: in one capture the
// same building shadow had a 15 unit penumbra, in the next (camera on the near plane,
// 61% of the map clamped) under 3. Store the true depth; clamp only the rasterised z.
namespace RtwCasterDepth
{
inline bool IsCasterEntry(const char* entry)
{
    if (!entry) return false;
    const std::string name = entry;
    return name.find("shadow_vs") != std::string::npos ||
           name == "tessellator_vs" || name == "tessellator_ds";
}

inline std::string Patch(const std::string& source, const char* entry)
{
    if (!IsCasterEntry(entry) || source.find("GetDistortedPosition") == std::string::npos ||
        source.find("DUST_RTW_TRUE_DEPTH") != std::string::npos)
        return source;

    std::string result = source;
    if (source.find("tessellator_ds") != std::string::npos)
    {
        // The tessellator clamps in the vertex shader, before the domain shader derives
        // the depth from the interpolated position. Clamp after that instead.
        const char* clamp = "oPosition.z = max(oPosition.z, 0.0);";
        const char* depth = "oDepth = oPosition.z;";
        size_t clampPos = result.find(clamp);
        size_t depthPos = result.find(depth);
        if (clampPos == std::string::npos || depthPos == std::string::npos || depthPos < clampPos)
            return source;
        result.insert(depthPos + std::char_traits<char>::length(depth),
                      "\n\toPosition.z = max(oPosition.z, 0.0); // [Dust] clamp after the true depth is taken");
        result.replace(clampPos, std::char_traits<char>::length(clamp), "/* [Dust] clamped in the domain shader */");
        return "#define DUST_RTW_TRUE_DEPTH 1\n" + result;
    }

    // <pos>.z = max(<pos>.z, 0.0); ...\n   <depth> = <pos>.z [/ <pos>.w];
    // Up to three unrelated statements may sit between the clamp and the depth.
    static const std::regex pattern(
        R"(([ \t]*)(\w+)\.z\s*=\s*max\(\s*\2\.z\s*,\s*0\.0f?\s*\)\s*;([^\n]*)\n((?:[^\n]*\n){0,3}?)([ \t]*)(\w+)\s*=\s*\2\.z(\s*/\s*\2\.w)?\s*;)");
    if (!std::regex_search(result, pattern)) return source;
    // Defer the clamp until the depth has been taken. Non-RTW (cascade) variants of
    // the same files keep the vanilla order.
    result = std::regex_replace(result, pattern,
        "#if !defined(RTW) && !defined(DUST_RTW_ONLY)\n"
        "$1$2.z = max($2.z, 0.0);$3\n"
        "#endif\n"
        "$4"
        "$5$6 = $2.z$7; // [Dust] RTW: true light depth, taken before the near clamp\n"
        "#if defined(RTW) || defined(DUST_RTW_ONLY)\n"
        "$5$2.z = max($2.z, 0.0);\n"
        "#endif");
    // rtwshadows.hlsl is RTW-only and never defines RTW.
    const bool rtwOnly = std::string(entry) == "rtw_shadow_vs";
    return std::string("#define DUST_RTW_TRUE_DEPTH 1\n") + (rtwOnly ? "#define DUST_RTW_ONLY 1\n" : "") + result;
}
}
