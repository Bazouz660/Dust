#pragma once
#include "DustAPI.h"
#include <algorithm>
#include <array>
#include <vector>

namespace PostProcessOrder {
inline DustInjectionPoint Point(const DustEffectDesc& desc) {
    if (desc.apiVersion >= 9 && desc.settings)
        for (uint32_t i = 0; i < desc.settingCount; ++i) {
            const auto& s = desc.settings[i];
            if ((s.settingFlags & DUST_SETTING_FLAG_POST_STAGE) && s.type == DUST_SETTING_ENUM && s.valuePtr)
                return *(const int*)s.valuePtr == 1 ? DUST_INJECT_POST_TONEMAP : DUST_INJECT_POST_LIGHTING;
        }
    return desc.injectionPoint;
}
inline int* OrderSetting(const DustEffectDesc& desc) {
    const auto point = Point(desc);
    const int flag = point == DUST_INJECT_POST_LIGHTING ? DUST_SETTING_FLAG_POST_ORDER_HDR :
                     point == DUST_INJECT_POST_TONEMAP ? DUST_SETTING_FLAG_POST_ORDER_LDR : 0;
    if (desc.apiVersion >= 9 && desc.settings)
        for (uint32_t i = 0; i < desc.settingCount; ++i) {
            const auto& s = desc.settings[i];
            if ((s.settingFlags & flag) && (s.type == DUST_SETTING_INT || s.type == DUST_SETTING_HIDDEN_INT) && s.valuePtr)
                return (int*)s.valuePtr;
        }
    return nullptr;
}

// Cache indices, keeping LoadedEffect objects and GUI snapshots stationary.
// Rebuild only when settings change. Fixed callbacks are barriers: e.g. LUT
// reconstructs LDR from HDR and must run before effects that consume that LDR.
class Schedule {
    struct Entry {
        DustInjectionPoint point;
        int priority, basePriority;
        bool movable, present;
        bool operator==(const Entry& b) const {
            return point == b.point && priority == b.priority && basePriority == b.basePriority &&
                   movable == b.movable && present == b.present;
        }
    };
    std::vector<Entry> entries_;
    std::array<std::vector<size_t>, 5> groups_;
public:
    template<class GetDesc> void Refresh(size_t count, GetDesc get) {
        bool changed = entries_.size() != count;
        entries_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& desc = get(i);
            const int* order = OrderSetting(desc);
            Entry e = { Point(desc), order ? *order : desc.priority, desc.priority,
                        order != nullptr, desc.postExecute != nullptr };
            if (!(entries_[i] == e)) { entries_[i] = e; changed = true; }
        }
        if (!changed) return;
        for (auto& group : groups_) group.clear();
        for (size_t i = 0; i < count; ++i)
            if (entries_[i].present && entries_[i].point >= 0 && entries_[i].point < groups_.size())
                groups_[entries_[i].point].push_back(i);
        for (auto& group : groups_) {
            std::stable_sort(group.begin(), group.end(), [&](size_t a, size_t b) {
                return entries_[a].basePriority < entries_[b].basePriority;
            });
            for (size_t begin = 0; begin < group.size();) {
                if (!entries_[group[begin]].movable) { ++begin; continue; }
                size_t end = begin + 1;
                while (end < group.size() && entries_[group[end]].movable) ++end;
                std::stable_sort(group.begin() + begin, group.begin() + end, [&](size_t a, size_t b) {
                    return entries_[a].priority < entries_[b].priority;
                });
                begin = end;
            }
        }
    }
    const std::vector<size_t>& Group(DustInjectionPoint point) const { return groups_[point]; }
    bool CanMove(size_t index, int direction) const {
        if (index >= entries_.size() || !entries_[index].movable || (direction != -1 && direction != 1)) return false;
        const auto& group = groups_[entries_[index].point];
        auto it = std::find(group.begin(), group.end(), index);
        if (it == group.end() || (direction < 0 ? it == group.begin() : it + 1 == group.end())) return false;
        return entries_[*(it + direction)].movable;
    }
    template<class GetDesc> bool Move(size_t index, int direction, GetDesc get) {
        if (!CanMove(index, direction)) return false;
        auto& group = groups_[entries_[index].point];
        size_t pos = std::find(group.begin(), group.end(), index) - group.begin();
        std::swap(group[pos], group[pos + direction]);
        size_t begin = pos, end = pos + 1;
        while (begin > 0 && entries_[group[begin - 1]].movable) --begin;
        while (end < group.size() && entries_[group[end]].movable) ++end;
        // Assign distinct ranks to the whole movable run, including tied INIs.
        for (size_t i = begin; i < end; ++i) *OrderSetting(get(group[i])) = int(i - begin);
        Refresh(entries_.size(), get);
        return true;
    }
};
}
