#pragma once

/** @file ProjectAiSettings.h
 * @brief Validated project policy for gameplay AI simulation and development controls.
 */
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/Application/ProjectSourceDocument.h"

namespace Horo::Application {
    /** @brief Execution profile supplied by the application composition owner. */
    enum class AiProjectRuntimeKind : std::uint8_t {
        Development,
        Packaged
    };

    /** @brief Operations requiring project authorization in a development profile. */
    enum class AiProjectDebugOperation : std::uint8_t {
        LiveMutation,
        ReplayCapture
    };

    /** @brief Validated authoritative project settings with no renderer or local visualization inputs. */
    class ProjectAiSettings final {
    public:
        /** @brief Returns the scheduler policy admitted by the runtime's validator. */
        [[nodiscard]] const AI::AiTaskSchedulerSettings &Scheduler() const noexcept {
            return scheduler_;
        }

        /** @brief Reports whether the project explicitly selected a scheduler mode. */
        [[nodiscard]] bool HasAuthoredSchedulerMode() const noexcept {
            return authoredSchedulerMode_;
        }

        /** @brief Returns the maximum authored Perception work units a Scene may request per tick. */
        [[nodiscard]] std::size_t PerceptionWorkUnitsLimit() const noexcept {
            return perceptionWorkUnitsLimit_;
        }

        /** @brief Returns the total work units available to environment queries in one authoritative tick. */
        [[nodiscard]] std::size_t QueryWorkUnitsLimit() const noexcept {
            return queryWorkUnitsLimit_;
        }

        /** @brief Checks the authored operation permission for a host-selected execution profile.
         * @param profile Exact execution profile supplied by the host.
         * @param operation Explicit operation requested by the application.
         * @return True only for a granted operation in a development profile.
         */
        [[nodiscard]] bool AllowsDebug(AiProjectRuntimeKind profile, AiProjectDebugOperation operation) const noexcept;

    private:
        friend Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &);

        ProjectAiSettings(AI::AiTaskSchedulerSettings scheduler, std::size_t perceptionWorkUnitsLimit, std::size_t queryWorkUnitsLimit,
                          bool mutation, bool capture, bool authoredSchedulerMode)
            : scheduler_(std::move(scheduler)), perceptionWorkUnitsLimit_(perceptionWorkUnitsLimit),
              queryWorkUnitsLimit_(queryWorkUnitsLimit), mutation_(mutation), capture_(capture),
              authoredSchedulerMode_(authoredSchedulerMode) {}

        AI::AiTaskSchedulerSettings scheduler_;
        std::size_t perceptionWorkUnitsLimit_;
        std::size_t queryWorkUnitsLimit_;
        bool mutation_{};
        bool capture_{};
        bool authoredSchedulerMode_{};
    };

    /** @brief Resolves versioned settings.ai from the canonical project source without creating a Scene.
     * @param source Strictly parsed project source document.
     * @return Validated authored policy, absence for older projects, or a typed policy diagnostic.
     */
    [[nodiscard]] Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &source);
}  // namespace Horo::Application
