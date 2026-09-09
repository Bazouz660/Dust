#include "../src/PostProcessOrder.h"
#include "../src/EffectSettingDefaults.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

// Compile the production dispatch bodies; substitute only config polling and
// timing (no disk, GPU or game required). Descriptors/callbacks are fixtures.
struct LoadedEffect { DustEffectDesc desc = {}; bool initialized = true; };
static std::vector<int> calls;
static int polls = 0;
static void (*onPoll)() = nullptr;
class EffectLoader {
public:
    std::vector<LoadedEffect> effects_;
    DustHostAPI hostAPI_ = {};
    PostProcessOrder::Schedule postSchedule_;
    uint64_t configPollFrame_ = UINT64_MAX;
    const std::vector<size_t>& GetPostOrder(DustInjectionPoint);
    bool CanMovePostEffect(size_t, int) const;
    bool MovePostEffect(size_t, int);
    void PrepareDispatch(uint64_t);
    void DispatchPre(DustInjectionPoint, const DustFrameContext*);
    void DispatchPost(DustInjectionPoint, const DustFrameContext*);
    void EffectConfigCheckHotReload(LoadedEffect&) { ++polls; if (onPoll) onPoll(); }
    void CollectTiming(LoadedEffect&, ID3D11DeviceContext*, int) {}
    void BeginTiming(LoadedEffect&, ID3D11DeviceContext*, int) {}
    void EndTiming(LoadedEffect&, ID3D11DeviceContext*, int) {}
};
#include "build/EffectDispatch.generated.h"

static int stage = 0;
static bool enabled = true;
int main() {
    int hdr = 40, ldr = 80, outline = 50, clarity = 50, dof = 75, bloom = 100;
    DustSettingDesc kuwahara[] = {
        {"Stage", DUST_SETTING_ENUM, &stage, 0, 1, "Stage", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_STAGE | DUST_SETTING_FLAG_PRESET_DEFAULT},
        {"HDR", DUST_SETTING_HIDDEN_INT, &hdr, -10000, 10000, "HDR", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_HDR | DUST_SETTING_FLAG_PRESET_DEFAULT},
        {"LDR", DUST_SETTING_HIDDEN_INT, &ldr, -10000, 10000, "LDR", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_LDR | DUST_SETTING_FLAG_PRESET_DEFAULT}
    };
    DustSettingDesc orders[] = {
        {"Outline", DUST_SETTING_HIDDEN_INT, &outline, -10000, 10000, "Order", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_HDR},
        {"Clarity", DUST_SETTING_HIDDEN_INT, &clarity, -10000, 10000, "Order", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_LDR},
        {"DoF", DUST_SETTING_HIDDEN_INT, &dof, -10000, 10000, "Order", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_LDR},
        {"Bloom", DUST_SETTING_HIDDEN_INT, &bloom, -10000, 10000, "Order", nullptr, nullptr, 0, DUST_SETTING_FLAG_POST_ORDER_LDR}
    };
    EffectLoader loader; loader.effects_.resize(7);
    const int priorities[] = {0,40,50,0,50,75,100};
    for (size_t i = 0; i < loader.effects_.size(); ++i) {
        auto& d = loader.effects_[i].desc; d.apiVersion = DUST_API_VERSION;
        d.injectionPoint = i < 3 ? DUST_INJECT_POST_LIGHTING : DUST_INJECT_POST_TONEMAP;
        d.priority = priorities[i]; d.flags = DUST_FLAG_FRAMEWORK_CONFIG;
    }
    auto& k = loader.effects_[1].desc; k.settings = kuwahara; k.settingCount = 3;
    k.IsEnabled = [] { return enabled ? 1 : 0; };
    k.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(1); };
    k.preExecute = [](const DustFrameContext* ctx, const DustHostAPI*) { assert(ctx->point == DUST_INJECT_POST_LIGHTING); calls.push_back(10); };
    loader.effects_[0].desc.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(0); };
    loader.effects_[2].desc.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(2); };
    loader.effects_[3].desc.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(3); }; // fixed LUT
    loader.effects_[4].desc.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(4); };
    loader.effects_[5].desc.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(5); };
    loader.effects_[6].desc.postExecute = [](const DustFrameContext*, const DustHostAPI*) { calls.push_back(6); };
    const size_t indices[] = {2,4,5,6};
    for (size_t j = 0; j < 4; ++j) { loader.effects_[indices[j]].desc.settings = &orders[j]; loader.effects_[indices[j]].desc.settingCount = 1; }
    EffectSettingDefaults defaults; defaults.Capture(k);
    auto frame = [&](uint64_t number) {
        calls.clear(); DustFrameContext ctx = {}; ctx.frameIndex = number;
        for (auto point : {DUST_INJECT_POST_LIGHTING, DUST_INJECT_POST_TONEMAP}) {
            ctx.point = point; loader.DispatchPre(point, &ctx); loader.DispatchPost(point, &ctx);
        }
    };
    frame(0); assert((calls == std::vector<int>{10,0,1,2,3,4,5,6})); assert(polls == 7);
    for (int i = 1; i <= 12; ++i) {
        stage = i % 2; frame(i);
        assert((calls == (stage ? std::vector<int>{10,0,2,3,4,5,1,6} : std::vector<int>{10,0,1,2,3,4,5,6})));
    }
    stage = 1; loader.GetPostOrder(DUST_INJECT_POST_TONEMAP);
    assert(loader.MovePostEffect(1, -1)); frame(13);
    assert((calls == std::vector<int>{10,0,2,3,4,1,5,6}));
    assert(loader.MovePostEffect(1, -1));
    assert(!loader.CanMovePostEffect(1, -1)); // cannot cross LUT
    assert(!loader.MovePostEffect(3, 1)); // fixed pass cannot move
    assert(!loader.MovePostEffect(2, 1)); // cannot cross stage boundary
    assert(!loader.MovePostEffect(100, 1));
    clarity = dof = ldr = bloom = 0; loader.GetPostOrder(DUST_INJECT_POST_TONEMAP);
    assert(loader.MovePostEffect(1, 1)); // tied ranks get distinct values
    frame(14); assert((calls == std::vector<int>{10,0,2,3,4,1,5,6}));
    // Config changes are read once at frame start even for a disabled effect.
    enabled = false; onPoll = [] { stage = 0; }; frame(15); onPoll = nullptr;
    assert((calls == std::vector<int>{0,2,3,4,5,6})); assert(polls == 7 * 16);
    enabled = true;
    for (uint32_t i = 0; i < k.settingCount; ++i) defaults.RestoreMissing(k.settings[i], i);
    assert(stage == 0 && hdr == 40 && ldr == 80);
    // Older plugins ignore scheduling metadata and keep fixed callbacks.
    k.apiVersion = 8; stage = 1;
    assert(PostProcessOrder::Point(k) == DUST_INJECT_POST_LIGHTING);
    assert(!PostProcessOrder::OrderSetting(k));
    std::puts("Production dispatch, stage switches, fixed barriers, tied ranks, pre callbacks and disabled hot reload passed");
}
