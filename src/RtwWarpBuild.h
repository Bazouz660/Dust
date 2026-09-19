#pragma once

#include <regex>
#include <string>

// Kenshi's RTWSM warp builder (rtwshadows.hlsl, rtw_build) writes 513 knots for a 512
// texel importance map. Knot k is the boundary between importance texels k-1 and k, so
// the knot AT the end of the important region (k == range.y) is the region's last point
// and belongs at +1.0. The game tests `my_u >= range.y` and sends it to the off-map
// value 1.05 together with everything beyond it, which stretches the region's last cell
// across the map border: in captures it ran from 0.85 to 1.024 in warped UV, and the part
// past 1.0 has no shadow map (receivers read lit, casters are clipped).
//
// That last cell is normally blur padding. It holds visible ground when the region
// touches the border of the light-space box, which is where the camera end of the view
// frustum always sits: zoomed in close, 8% and 18% of the visible pixels fell off the map
// in two captures, as a straight band with no shadows ("cut" shadows). With `>` the knot
// takes the ordinary branch, where the remaining weight is zero and it lands on +1.0.
namespace RtwWarpBuild
{
inline std::string Patch(const std::string& source)
{
    static const std::regex pattern(R"(else\s+if\s*\(\s*my_u\s*>=\s*range\.y\s*\))");
    if (source.find("rtw_build") == std::string::npos || !std::regex_search(source, pattern))
        return source;
    return std::regex_replace(source, pattern, "else if(my_u > range.y) /* [Dust] the end knot is inside the region */");
}
}
