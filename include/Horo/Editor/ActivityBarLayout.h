#pragma once

#include "Horo/Editor/WorkspaceLayout.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    enum class ActivityBarRail : std::uint8_t {
        Left,
        Right,
        Bottom,
        DocumentTop,
    };

    struct ActivityBarSlot {
        ActivityBarRail rail = ActivityBarRail::Left;
        std::size_t groupIndex = 0;
        std::size_t itemIndex = 0;

        friend bool operator==(const ActivityBarSlot &, const ActivityBarSlot &) = default;
    };

    struct ActivityBarGroup {
        std::vector<PanelId> items;
    };

    enum class ActivityBarLayoutOperationCode : std::uint8_t {
        Success,
        NoOp,
        UnknownItem,
        DuplicateItem,
        InvalidGroup,
        InvalidInsertionIndex,
    };

    struct ActivityBarLayoutOperationResult {
        ActivityBarLayoutOperationCode code = ActivityBarLayoutOperationCode::Success;

        [[nodiscard]] bool Succeeded() const noexcept {
            return code == ActivityBarLayoutOperationCode::Success;
        }
    };

    class ActivityBarLayout {
    public:
        static constexpr std::size_t kDefaultGroupCount = 3;

        ActivityBarLayout();

        [[nodiscard]] ActivityBarLayoutOperationResult Insert(std::string_view panelId, ActivityBarSlot slot);
        [[nodiscard]] ActivityBarLayoutOperationResult Move(std::string_view panelId, ActivityBarSlot slot);
        /**
         * @brief Withdraws an item from its rail and compacts the remaining group.
         * @param panelId Stable identity of the item to withdraw.
         * @return Success when removed, or UnknownItem without mutation when absent.
         * @note Removes presentation placement only; the host retains provider and drawer ownership.
         */
        [[nodiscard]] ActivityBarLayoutOperationResult Remove(std::string_view panelId);

        [[nodiscard]] std::optional<ActivityBarSlot> FindSlot(std::string_view panelId) const;
        [[nodiscard]] std::string_view ItemAt(ActivityBarRail rail, std::size_t groupIndex, std::size_t itemIndex) const noexcept;
        [[nodiscard]] const std::vector<ActivityBarGroup> &Groups(ActivityBarRail rail) const noexcept;

    private:
        using RailGroups = std::array<std::vector<ActivityBarGroup>, 4>;

        [[nodiscard]] static std::size_t RailIndex(ActivityBarRail rail) noexcept;
        [[nodiscard]] ActivityBarGroup *GetGroup(ActivityBarSlot slot) noexcept;
        [[nodiscard]] const ActivityBarGroup *GetGroup(ActivityBarSlot slot) const noexcept;

        RailGroups m_groups;
    };
}  // namespace Horo::Editor
