#include "../src/EffectDragDrop.h"
#include "../src/PostProcessOrder.h"
#include <cassert>
#include <cstdio>

// Real vendored ImGui frames and mouse input, without opening a native window.
// Exercise the exact header drag/drop widget used by the settings list.
struct Fixture {
    DustEffectDesc effects[6] = {};
    DustSettingDesc settings[6] = {};
    int priorities[6] = {40,50,0,75,50,100};
    const char* names[6] = {"Kuwahara", "Outline", "LUT", "DoF", "Clarity", "Bloom"};
    ImVec2 top[6], bottom[6];
    bool open[6] = {};
    PostProcessOrder::Schedule schedule;
    std::vector<size_t> visual;
    int deliveries = 0;
    Fixture() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800,800); io.DeltaTime = 1.f/60;
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        unsigned char* pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
        for (size_t i = 0; i < 6; ++i) {
            auto& d = effects[i]; d.apiVersion = DUST_API_VERSION; d.name = names[i];
            d.injectionPoint = i < 2 ? DUST_INJECT_POST_LIGHTING : DUST_INJECT_POST_TONEMAP;
            d.priority = priorities[i]; d.postExecute = [](const DustFrameContext*, const DustHostAPI*) {};
            settings[i].type = DUST_SETTING_HIDDEN_INT; settings[i].valuePtr = &priorities[i];
            settings[i].settingFlags = i < 2 ? DUST_SETTING_FLAG_POST_ORDER_HDR : DUST_SETTING_FLAG_POST_ORDER_LDR;
            if (i != 2) { d.settings = &settings[i]; d.settingCount = 1; }
        }
    }
    ~Fixture() { ImGui::DestroyContext(); }
    void Frame(ImVec2 mouse = ImVec2(-100,-100), bool down = false) {
        auto& io = ImGui::GetIO(); io.MousePos = mouse; io.MouseDown[0] = down;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(700,750));
        ImGui::Begin("Effects", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        schedule.Refresh(6, [&](size_t i) -> const DustEffectDesc& { return effects[i]; });
        EffectDragDrop::Drop drop; visual.clear();
        for (auto point : {DUST_INJECT_POST_LIGHTING, DUST_INJECT_POST_TONEMAP}) {
            ImGui::CollapsingHeader(point == DUST_INJECT_POST_LIGHTING ? "HDR" : "LDR", ImGuiTreeNodeFlags_DefaultOpen);
            const auto order = schedule.Group(point);
            for (size_t i : order) {
                visual.push_back(i); ImGui::PushID((int)i);
                ImGui::SetNextItemOpen(i == 3, ImGuiCond_Once); // DoF expanded, other effects collapsed
                open[i] = ImGui::CollapsingHeader(names[i]);
                top[i] = ImGui::GetItemRectMin(); bottom[i] = ImGui::GetItemRectMax();
                EffectDragDrop::Header(i, names[i], i != 2,
                    [&](size_t a, size_t b, bool after) { return schedule.CanPlace(a,b,after); }, drop);
                if (open[i]) {
                    float strength = .5f; ImGui::SliderFloat("Strength", &strength, 0, 1);
                    ImGui::TextUnformatted("Expanded effect settings");
                }
                ImGui::PopID();
            }
        }
        if (drop.pending) {
            assert(schedule.Place(drop.source,drop.target,drop.after,[&](size_t i) -> const DustEffectDesc& { return effects[i]; }));
            ++deliveries;
        }
        ImGui::End(); ImGui::Render();
    }
    ImVec2 Position(size_t index, bool lower = false) {
        return ImVec2(top[index].x + 120, top[index].y + (bottom[index].y-top[index].y)*(lower ? .8f : .2f));
    }
    void Drag(size_t source, size_t target, bool after) {
        const auto start = Position(source), end = Position(target,after);
        Frame(start); Frame(start,true);
        Frame(ImVec2(start.x+20,start.y),true); // exceed drag threshold
        Frame(end,true); Frame(end,true); Frame(end,false); Frame();
    }
};
int main() {
    Fixture f; f.Frame(); f.Frame();
    assert((f.visual == std::vector<size_t>{0,1,2,4,3,5}));
    assert(f.open[3]);
    f.Drag(3,5,true); // expanded DoF moves with its settings
    assert(f.deliveries == 1);
    assert((f.visual == std::vector<size_t>{0,1,2,4,5,3})); assert(f.open[3]);
    f.Drag(5,4,false); // collapsed Bloom can also be dragged
    assert(f.deliveries == 2);
    assert((f.visual == std::vector<size_t>{0,1,2,5,4,3}));
    f.Drag(5,0,false); assert(f.deliveries == 2); // cross-group rejection
    f.Drag(5,2,false); assert(f.deliveries == 2); // fixed target
    f.Drag(2,3,true); assert(f.deliveries == 2);  // fixed source
    assert((f.visual == std::vector<size_t>{0,1,2,5,4,3}));
    std::puts("Real ImGui mouse drags: before/after, expanded/collapsed settings, visual order and invalid drops passed");
}
