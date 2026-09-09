#include "EffectShaderProbe.h"
#include "../src/EffectSettingDefaults.h"
#include <algorithm>

static std::vector<Pixel> Pattern(float base, float detail) {
    std::vector<Pixel> image(EffectShaderProbe::W * EffectShaderProbe::H);
    for (UINT y = 0; y < EffectShaderProbe::H; ++y) for (UINT x = 0; x < EffectShaderProbe::W; ++x) {
        float v = base + ((x+y)%2 ? detail : 0);
        image[y*EffectShaderProbe::W+x] = {v,v,v,1};
    }
    return image;
}
// Independent reference for radius-1 Gaussian blur, including the two UNORM
// intermediate writes. Protect=0 and MidtoneProtect=0 must match the old path.
static std::vector<Pixel> Legacy(const std::vector<Pixel>& input) {
    auto h = input, v = input, result = input;
    const double side = std::exp(-2.0) / (1.0 + 2.0*std::exp(-2.0));
    auto sample = [](const std::vector<Pixel>& image, int x, int y, int c) {
        return image[std::clamp(y,0,int(EffectShaderProbe::H)-1)*EffectShaderProbe::W + std::clamp(x,0,int(EffectShaderProbe::W)-1)][c];
    };
    for (int axis = 0; axis < 2; ++axis) {
        const auto& src = axis ? h : input; auto& dst = axis ? v : h;
        for (int y = 0; y < EffectShaderProbe::H; ++y) for (int x = 0; x < EffectShaderProbe::W; ++x) for (int c = 0; c < 3; ++c) {
            double value = sample(src,x,y,c)*(1-2*side) +
                (sample(src,x-(axis==0),y-(axis==1),c)+sample(src,x+(axis==0),y+(axis==1),c))*side;
            dst[y*EffectShaderProbe::W+x][c] = float(std::round(value*255)/255);
        }
    }
    for (size_t i = 0; i < input.size(); ++i) for (int c = 0; c < 3; ++c)
        result[i][c] = std::clamp(input[i][c] + (input[i][c]-v[i][c])*.4f, 0.f, 1.f);
    return result;
}
int main() {
    EffectShaderProbe p("clarity", "DustClarity.dll");
    EffectSettingDefaults defaults; defaults.Capture(p.effect);
    p.Setting<float>("BlurRadius") = 1;
    p.Setting<float>("MidtoneProtect") = 0;
    auto night = Pattern(.015f,.03f), daylight = Pattern(.4f,.1f), middle = Pattern(.08f,.04f);
    auto legacyNight = p.Render(night, 0), legacyDay = p.Render(daylight, 0), legacyMiddle = p.Render(middle, 0);
    AssertImagesNear(legacyNight, Legacy(night)); AssertImagesNear(legacyDay, Legacy(daylight));
    assert(legacyNight[1][0] > night[1][0]); // reproduce dark-detail brightening
    p.Setting<float>("LuminanceProtect") = 1;
    AssertImagesNear(p.Render(night, 0), night);
    AssertImagesNear(p.Render(daylight, 0), legacyDay);
    p.Setting<float>("LuminanceStart") = .05f; p.Setting<float>("LuminanceEnd") = .15f;
    auto expected = middle;
    for (size_t i = 0; i < middle.size(); ++i) {
        float t = std::clamp((middle[i][0]-.05f)/.1f, 0.f, 1.f); t = t*t*(3-2*t);
        for (int c = 0; c < 3; ++c) expected[i][c] += (legacyMiddle[i][c]-middle[i][c])*t;
    }
    auto masked = p.Render(middle, 0); AssertImagesNear(masked, expected);
    p.Setting<float>("LuminanceProtect") = .5f;
    for (size_t i = 0; i < expected.size(); ++i) for (int c = 0; c < 3; ++c) expected[i][c] = (masked[i][c]+legacyMiddle[i][c])*.5f;
    AssertImagesNear(p.Render(middle, 0), expected);
    p.Setting<float>("LuminanceProtect") = 1;
    p.Setting<float>("LuminanceStart") = .15f; p.Setting<float>("LuminanceEnd") = .05f;
    AssertImagesNear(p.Render(middle, 0), masked);
    p.Setting<float>("LuminanceStart") = .05f;
    AssertImagesNear(p.Render(night, 0), night); AssertImagesNear(p.Render(daylight, 0), legacyDay);
    p.Setting<bool>("DebugView") = true;
    auto debug = p.Render(middle, 0); p.Setting<float>("LuminanceProtect") = 0;
    AssertImagesNear(p.Render(middle, 0), debug); // raw detail remains inspectable
    p.Setting<bool>("DebugView") = false; p.Setting<float>("LuminanceProtect") = 1;
    for (uint32_t i = 0; i < p.effect.settingCount; ++i) defaults.RestoreMissing(p.effect.settings[i], i);
    AssertImagesNear(p.Render(night, 0), legacyNight);
    std::puts("Legacy reference, dark pixels, daylight, per-pixel ramp, partial protection, thresholds and debug passed");
}
