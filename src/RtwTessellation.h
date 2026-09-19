#pragma once

#include <string>

// Kenshi tessellates its standard RTWSM shadow casters (rtwtessellator.hlsl) because
// casters are warped per vertex while receivers look the warp up per pixel. Its hull
// shader sizes each edge by the distance between the two ALREADY WARPED end points
// (floor(1 + 10 * length)). An edge that crosses the high-resolution zone but ends in
// the collapsed zones on either side therefore gets almost no subdivision, and a wall
// is rasterised as a straight line between two badly placed end points: the shadow is
// cut, most visibly when zoomed in, where the high-resolution zone is small. The same
// metric splits every triangle at least once (inside factor >= 2).
namespace RtwTessellation
{
// Subdivisions per warp cell. The warp is piecewise linear per axis over 513 knots (512 cells);
// on captured geometry x3 brings the worst edge error from 40 to 4 shadow texels (at
// 2048) while emitting 9.4M triangles where the vanilla metric emits 15.0M.
constexpr int SegmentsPerCell = 3;

inline std::string Patch(const std::string& source)
{
    const char* begin = "float max_edge_len";
    const char* end = "inside";
    const char* function = "void constant_hs";
    size_t functionPos = source.find(function);
    size_t from = source.find(begin, functionPos == std::string::npos ? 0 : functionPos);
    if (functionPos == std::string::npos || from == std::string::npos ||
        source.find("DustRtwEdgeFactor") != std::string::npos)
        return source;
    size_t inside = source.find(end, source.find("edges[2]", from));
    size_t to = inside == std::string::npos ? inside : source.find(';', inside);
    if (to == std::string::npos) return source;

    const std::string helper =
        "float DustRtwEdgeFactor(VSOutput a, VSOutput b) {\n"
        // The warp is monotonic per axis: an edge whose warped end points are both
        // beyond the same map border never enters the map.
        "\tfloat2 lo = min(a.p2d.xy, b.p2d.xy);\n"
        "\tfloat2 hi = max(a.p2d.xy, b.p2d.xy);\n"
        "\tif (hi.x < -1.0 || hi.y < -1.0 || lo.x > 1.0 || lo.y > 1.0) return 1.0;\n"
        // position is the UNWARPED clip position. Only symmetric operations, so both
        // triangles sharing an edge agree on its factor.
        "\tfloat2 span = abs(a.position.xy / a.position.w - b.position.xy / b.position.w) * (0.5 * 512.0);\n"
        "\treturn clamp(ceil(max(span.x, span.y) * " + std::to_string(SegmentsPerCell) + ".0), 1.0, 64.0);\n"
        "}\n\n";
    const std::string body =
        "edges[0] = DustRtwEdgeFactor(patch[1], patch[2]);\n"
        "\tedges[1] = DustRtwEdgeFactor(patch[2], patch[0]);\n"
        "\tedges[2] = DustRtwEdgeFactor(patch[0], patch[1]);\n"
        "\tinside   = max(edges[0], max(edges[1], edges[2]));";

    std::string result = source;
    result.replace(from, to + 1 - from, body);
    result.insert(functionPos, helper);
    return result;
}
}
