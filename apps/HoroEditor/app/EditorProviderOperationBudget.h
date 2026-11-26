#pragma once

/** @file EditorProviderOperationBudget.h @brief Editor-lane admission for model and speech provider operations. */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace Horo::Editor {
    /** @brief Independent operation ceilings for the editor process and each project within it. */
    struct EditorProviderOperationLimits final {
        std::optional<std::uint32_t> session; /**< Unset leaves the session uncapped. */
        std::optional<std::uint32_t> project; /**< Unset leaves individual projects uncapped. */
    };

    /** @brief Result of asking the budget to start an operation. */
    enum class EditorProviderAdmission {
        Started,
        SessionExhausted,
        ProjectExhausted,
        NotStarted
    };

    /** @brief Shares quota across providers and workspace attachments on the editor owner lane. */
    class EditorProviderOperationBudget final {
    public:
        explicit EditorProviderOperationBudget(const EditorProviderOperationLimits limits) noexcept : limits_(limits) {}

        /** @brief Invokes start only within both quotas, and charges only if it accepts the operation.
         * @param project Stable identity of the attached project.
         * @param start Synchronous dispatch that returns true once it owns the operation. Must not reenter this budget.
         * @return The exhausted scope, a rejected start, or a charged start.
         */
        template <typename Start> EditorProviderAdmission TryStart(const std::string &project, Start &&start) {
            using enum EditorProviderAdmission;
            if (limits_.session && sessionOperations_ >= *limits_.session)
                return SessionExhausted;
            if (limits_.project && ProjectOperations(project) >= *limits_.project)
                return ProjectExhausted;
            auto *projectOperations = limits_.project ? &projectOperations_[project] : nullptr;
            if (!std::forward<Start>(start)())
                return NotStarted;
            if (limits_.session)
                ++sessionOperations_;
            if (projectOperations)
                ++*projectOperations;
            return Started;
        }

        /** @brief Returns the configured ceilings. */
        [[nodiscard]] const EditorProviderOperationLimits &Limits() const noexcept {
            return limits_;
        }
        /** @brief Returns the accepted operations counted against the session ceiling. */
        [[nodiscard]] std::uint32_t SessionOperations() const noexcept {
            return sessionOperations_;
        }
        /** @brief Returns the accepted operations counted against this project's ceiling. */
        [[nodiscard]] std::uint32_t ProjectOperations(const std::string &project) const {
            const auto used = projectOperations_.find(project);
            return used == projectOperations_.end() ? 0 : used->second;
        }

    private:
        EditorProviderOperationLimits limits_;
        std::map<std::string, std::uint32_t> projectOperations_;
        std::uint32_t sessionOperations_{};
    };
}  // namespace Horo::Editor
