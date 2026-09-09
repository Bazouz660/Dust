#include "EffectShaderProbe.h"
#include "../src/EffectSettingDefaults.h"
#include "../src/PostProcessOrder.h"
#include <algorithm>

// Independent CPU reference for the original integer-radius sector filter.
static std::vector<Pixel> Legacy(const std::vector<Pixel>& input, int radius) {
    std::vector<Pixel> result(input.size());
    const float dirs[8][2] = {{1,0},{.707f,.707f},{0,1},{-.707f,.707f},{-1,0},{-.707f,-.707f},{0,-1},{.707f,-.707f}};
    for (int y = 0; y < EffectShaderProbe::H; ++y) for (int x = 0; x < EffectShaderProbe::W; ++x) {
        double sum[8][3] = {}, square[8][3] = {}; int count[8] = {};
        for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
            double length = std::sqrt(double(dx*dx + dy*dy));
            if (length > radius + .5) continue;
            auto p = input[std::clamp(y+dy, 0, int(EffectShaderProbe::H)-1)*EffectShaderProbe::W + std::clamp(x+dx, 0, int(EffectShaderProbe::W)-1)];
            for (int s = 0; s < 8; ++s) if (length == 0 || (dx*dirs[s][0]+dy*dirs[s][1])/length >= .383) {
                ++count[s];
                for (int c = 0; c < 3; ++c) { sum[s][c] += p[c]; square[s][c] += double(p[c])*p[c]; }
            }
        }
        double minVar = 1e20; Pixel chosen = {};
        for (int s = 0; s < 8; ++s) {
            double var = 0; Pixel mean = {};
            for (int c = 0; c < 3; ++c) { mean[c] = float(sum[s][c]/count[s]); var += std::abs(square[s][c]/count[s] - (sum[s][c]/count[s])*(sum[s][c]/count[s])); }
            if (var < minVar) { minVar = var; chosen = mean; }
        }
        chosen[3] = 1; result[y*EffectShaderProbe::W+x] = chosen;
    }
    return result;
}

int main() {
    EffectShaderProbe p("kuwahara", "DustKuwahara.dll");
    EffectSettingDefaults defaults; defaults.Capture(p.effect);
    std::vector<Pixel> input(p.W*p.H);
    uint32_t random = 123;
    for (auto& pixel : input) { for (int c = 0; c < 3; ++c) { random = random*1664525+1013904223; pixel[c] = .02f + float(random >> 8)/16777216.f; } pixel[3] = 1; }
    p.Setting<int>("Radius") = 3;
    auto legacy = p.Render(input, .01f);
    AssertImagesNear(legacy, Legacy(input, 3));
    p.Setting<bool>("DepthEnabled") = true;
    p.Setting<float>("DepthStart") = .1f; p.Setting<float>("DepthEnd") = .5f;
    AssertImagesNear(p.Render(input, .05f), input);
    AssertImagesNear(p.Render(input, .6f), legacy);
    AssertImagesNear(p.Render(input, 0), legacy); // sky
    // Spatial depth fixture: far objects at the top, nearby detail at the
    // bottom. Catch flipped/misaligned depth sampling, not just scalar math.
    std::vector<float> distances(p.W * p.H);
    auto spatialExpected = input;
    for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x) {
        const size_t i = y * p.W + x;
        distances[i] = y < p.H / 2 ? .6f : .05f;
        if (y < p.H / 2) spatialExpected[i] = legacy[i];
    }
    AssertImagesNear(p.RenderDepths(input, distances), spatialExpected);
    p.Setting<bool>("DepthPreview") = true;
    p.Setting<float>("Strength") = 0; // preview must bypass the zero-effect early return
    for (UINT y = 0; y < p.H; ++y) for (UINT x = 0; x < p.W; ++x) {
        float fade = y < p.H / 2 ? 1.f : 0.f;
        spatialExpected[y*p.W+x] = {fade,fade,fade,1};
    }
    AssertImagesNear(p.RenderDepths(input, distances), spatialExpected);
    p.hasDepth = false;
    AssertImagesNear(p.Render(input,.1f), std::vector<Pixel>(p.W*p.H, Pixel{1,0,1,1}));
    p.hasDepth = true; p.Setting<bool>("DepthPreview") = false; p.Setting<float>("Strength") = 1;
    p.Setting<float>("NearRadius") = 3;
    auto halfway = p.Render(input, .3f);
    auto expected = legacy;
    for (size_t i = 0; i < input.size(); ++i) for (int c = 0; c < 3; ++c) expected[i][c] = (input[i][c]+legacy[i][c])*.5f;
    AssertImagesNear(halfway, expected);
    p.Setting<float>("Strength") = 0; p.Setting<float>("NearStrength") = 1;
    AssertImagesNear(p.Render(input, .05f), legacy); // far zero must not skip near effect
    AssertImagesNear(p.Render(input, .6f), input);
    p.Setting<float>("Strength") = 1; p.Setting<float>("NearStrength") = 0; p.Setting<float>("NearRadius") = 0;
    p.Setting<float>("DepthStart") = .5f; p.Setting<float>("DepthEnd") = .1f;
    AssertImagesNear(p.Render(input, .05f), input); AssertImagesNear(p.Render(input, .6f), legacy);
    p.Setting<float>("DepthStart") = .1f; p.Setting<float>("DepthEnd") = .1f;
    AssertImagesNear(p.Render(input, .05f), input); AssertImagesNear(p.Render(input, .6f), legacy);
    p.hasDepth = false; AssertImagesNear(p.Render(input, .05f), legacy);
    p.hasDepth = true; p.Setting<float>("NearStrength") = 1;
    // Fractional-radius boundary must not jump when one additional ring is introduced.
    p.Setting<float>("NearRadius") = 1.99999f; auto below = p.Render(input, .05f);
    p.Setting<float>("NearRadius") = 2.00001f; AssertImagesNear(p.Render(input, .05f), below, .001f);
    for (uint32_t i = 0; i < p.effect.settingCount; ++i) defaults.RestoreMissing(p.effect.settings[i], i);
    assert(!p.Setting<bool>("DepthEnabled")); AssertImagesNear(p.Render(input, .05f), legacy);
    for (int i = 0; i < 12; ++i) {
        p.Setting<int>("RenderStage") = i % 2;
        const auto point = PostProcessOrder::Point(p.effect);
        AssertImagesNear(p.Render(input, .05f, point), legacy);
        assert(p.lastTarget == (i % 2 ? DUST_RESOURCE_LDR_RT : DUST_RESOURCE_HDR_RT));
    }
    std::puts("Depth endpoints, legacy kernels, blend, sky, missing depth, fractional radii and preset defaults passed");
}
