#pragma once

#include <regex>
#include <string>

// Temporal AA renders the G-buffer through a viewport shifted by a sub-pixel jitter, so
// pixel p holds the scene sample the unjittered view would have at p - jitter. The
// game's deferred sun pass rebuilds the world position from the view ray through the
// centre of p. The point it gets is off the real surface by the jitter's footprint,
// differently every frame; on ground seen at a grazing angle that is several world
// units along the light, far more than the 0.00003 shadow bias: acne that flickers with
// the jitter. Rebuild along the ray the sample was actually taken on. `ray` is a linear
// screen-space interpolant, so ddx/ddy give its exact per-pixel step.
namespace DeferredJitter
{
// PS b8, bound by the host for the deferred sun draw only. Unbound reads as zero: no
// correction and static sampling noise, i.e. the behaviour without temporal AA.
constexpr unsigned Slot = 8;

struct FrameParams
{
    float jitterX, jitterY;   // this frame's G-buffer viewport jitter in pixels
    float frame;              // frame counter modulo 64 while temporal AA accumulates, else 0
    float temporal;           // 1 while a temporal AA / upscaler is integrating frames
};

inline const char* Declaration()
{
    return "// [Dust] Per-frame parameters (bound by the host at b8 for the sun pass)\n"
           "cbuffer DustFrameParams : register(b8) {\n"
           "\tfloat2 dustJitterPx;\n"
           "\tfloat dustFrame;\n"
           "\tfloat dustTemporal;\n"
           "};\n\n";
}

// Replaces the FIRST reconstruction only: main_fs precedes light_fs in deferred.hlsl.
inline std::string Patch(const std::string& source)
{
    static const std::regex pattern(
        R"(float4\s+viewPos\s*=\s*float4\s*\(\s*normalize\s*\(\s*ray\s*\)\s*\*\s*distance\s*,\s*1\.0f?\s*\)\s*;)");
    if (source.find("dustJitterRay") != std::string::npos) return source;
    return std::regex_replace(source, pattern,
        "float3 dustJitterRay = ray - dustJitterPx.x * ddx(ray) - dustJitterPx.y * ddy(ray);\n"
        "\tfloat4 viewPos  = float4( normalize(dustJitterRay) * distance, 1.0f);",
        std::regex_constants::format_first_only);
}
}
