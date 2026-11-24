#include "Horo/Editor/ActivityBarLayout.h"

#include <algorithm>

namespace Horo::Editor {
    ActivityBarLayout::ActivityBarLayout() {
        using enum ActivityBarRail;
        m_groups[RailIndex(Left)].resize(kDefaultGroupCount);
        m_groups[RailIndex(Right)].resize(kDefaultGroupCount);
        m_groups[RailIndex(Bottom)].resize(kDefaultGroupCount);
        m_groups[RailIndex(DocumentTop)].resize(1);
    }

    std::size_t ActivityBarLayout::RailIndex(const ActivityBarRail rail) noexcept {
        return static_cast<std::size_t>(rail);
    }

    ActivityBarGroup *ActivityBarLayout::GetGroup(const ActivityBarSlot slot) noexcept {
        auto &rail = m_groups[RailIndex(slot.rail)];
        return slot.groupIndex < rail.size() ? &rail[slot.groupIndex] : nullptr;
    }

    const ActivityBarGroup *ActivityBarLayout::GetGroup(const ActivityBarSlot slot) const noexcept {
        const auto &rail = m_groups[RailIndex(slot.rail)];
        return slot.groupIndex < rail.size() ? &rail[slot.groupIndex] : nullptr;
    }

    ActivityBarLayoutOperationResult ActivityBarLayout::Insert(const std::string_view panelId, const ActivityBarSlot slot) {
        using enum ActivityBarLayoutOperationCode;
        if (panelId.empty()) {
            return {UnknownItem};
        }
        if (FindSlot(panelId).has_value()) {
            return {DuplicateItem};
        }

        ActivityBarGroup *group = GetGroup(slot);
        if (group == nullptr) {
            return {InvalidGroup};
        }
        if (slot.itemIndex > group->items.size()) {
            return {InvalidInsertionIndex};
        }

        group->items.emplace(group->items.begin() + static_cast<std::ptrdiff_t>(slot.itemIndex), panelId);
        return {};
    }

    ActivityBarLayoutOperationResult ActivityBarLayout::Move(const std::string_view panelId, const ActivityBarSlot slot) {
        using enum ActivityBarLayoutOperationCode;
        const auto sourceSlot = FindSlot(panelId);
        if (!sourceSlot.has_value()) {
            return {UnknownItem};
        }

        const ActivityBarGroup *targetGroup = GetGroup(slot);
        if (targetGroup == nullptr) {
            return {InvalidGroup};
        }
        if (slot.itemIndex > targetGroup->items.size()) {
            return {InvalidInsertionIndex};
        }

        ActivityBarGroup *sourceGroup = GetGroup(*sourceSlot);
        ActivityBarGroup *mutableTargetGroup = GetGroup(slot);
        const std::size_t sourceIndex = sourceSlot->itemIndex;
        const std::size_t requestedIndex = slot.itemIndex;

        if (sourceGroup == mutableTargetGroup && (requestedIndex == sourceIndex || requestedIndex == sourceIndex + 1)) {
            return {NoOp};
        }

        PanelId moved = std::move(sourceGroup->items[sourceIndex]);
        sourceGroup->items.erase(sourceGroup->items.begin() + static_cast<std::ptrdiff_t>(sourceIndex));

        const std::size_t adjustedIndex =
            sourceGroup == mutableTargetGroup && requestedIndex > sourceIndex ? requestedIndex - 1 : requestedIndex;
        mutableTargetGroup->items.emplace(mutableTargetGroup->items.begin() + static_cast<std::ptrdiff_t>(adjustedIndex), std::move(moved));
        return {};
    }

    /** @copydoc ActivityBarLayout::Remove */
    ActivityBarLayoutOperationResult ActivityBarLayout::Remove(const std::string_view panelId) {
        const auto slot = FindSlot(panelId);
        if (!slot.has_value()) {
            return {ActivityBarLayoutOperationCode::UnknownItem};
        }
        ActivityBarGroup *group = GetGroup(*slot);
        group->items.erase(group->items.begin() + static_cast<std::ptrdiff_t>(slot->itemIndex));
        return {};
    }

    std::optional<ActivityBarSlot> ActivityBarLayout::FindSlot(const std::string_view panelId) const {
        for (std::size_t railIndex = 0; railIndex < m_groups.size(); ++railIndex) {
            for (std::size_t groupIndex = 0; groupIndex < m_groups[railIndex].size(); ++groupIndex) {
                const auto &items = m_groups[railIndex][groupIndex].items;
                if (const auto item = std::ranges::find(items, panelId); item != items.end()) {
                    return ActivityBarSlot{static_cast<ActivityBarRail>(railIndex), groupIndex,
                                           static_cast<std::size_t>(std::distance(items.begin(), item))};
                }
            }
        }
        return std::nullopt;
    }

    std::string_view ActivityBarLayout::ItemAt(const ActivityBarRail rail, const std::size_t groupIndex,
                                               const std::size_t itemIndex) const noexcept {
        const ActivityBarGroup *group = GetGroup(ActivityBarSlot{rail, groupIndex, itemIndex});
        if (group == nullptr || itemIndex >= group->items.size()) {
            return {};
        }
        return group->items[itemIndex];
    }

    const std::vector<ActivityBarGroup> &ActivityBarLayout::Groups(const ActivityBarRail rail) const noexcept {
        return m_groups[RailIndex(rail)];
    }
}  // namespace Horo::Editor
