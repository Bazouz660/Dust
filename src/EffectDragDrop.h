#pragma once
#include "imgui/imgui.h"
#include <cstddef>
#include <cstring>

namespace EffectDragDrop {
struct Drop {
    size_t source = 0, target = 0;
    bool after = false, pending = false;
};
constexpr const char* PayloadType = "DUST_EFFECT_SECTION";

// Call immediately after the effect header, including when it is collapsed.
// Keep mutation deferred until the list is finished, so the current frame
// cannot duplicate/skip sections or move the widget under the mouse.
template<class CanPlace>
void Header(size_t index, const char* name, bool movable, CanPlace canPlace, Drop& drop) {
    const ImVec2 top = ImGui::GetItemRectMin(), bottom = ImGui::GetItemRectMax();
    if (movable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        ImGui::SetDragDropPayload(PayloadType, &index, sizeof(index));
        ImGui::TextUnformatted(name);
        ImGui::EndDragDropSource();
    }
    const auto* current = ImGui::GetDragDropPayload();
    if (!current || !current->IsDataType(PayloadType) || current->DataSize != sizeof(size_t)) return;
    size_t source = 0;
    std::memcpy(&source, current->Data, sizeof(source));
    const bool after = ImGui::GetIO().MousePos.y >= (top.y + bottom.y) * 0.5f;
    if (!canPlace(source, index, after) || !ImGui::BeginDragDropTarget()) return;
    if (const auto* payload = ImGui::AcceptDragDropPayload(PayloadType,
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const float y = after ? bottom.y : top.y;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(top.x, y), ImVec2(bottom.x, y),
            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 3.0f);
        if (payload->IsDelivery()) drop = { source, index, after, true };
    }
    ImGui::EndDragDropTarget();
}

inline void AutoScroll() {
    const auto* payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType(PayloadType)) return;
    const ImVec2 top = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < top.x || mouse.x > top.x + size.x || mouse.y < top.y || mouse.y > top.y + size.y) return;
    const float edge = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    float direction = mouse.y < top.y + edge ? -1.0f : mouse.y > top.y + size.y - edge ? 1.0f : 0.0f;
    if (direction != 0.0f)
        ImGui::SetScrollY(ImGui::GetScrollY() + direction * 500.0f * ImGui::GetIO().DeltaTime);
}
}
