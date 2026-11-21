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

        /** @brief Checks the authored operation permission for a host-selected execution profile.
         * @param profile Exact execution profile supplied by the host.
         * @param operation Explicit operation requested by the application.
         * @return True only for a granted operation in a development profile.
         */
        [[nodiscard]] bool AllowsDebug(AiProjectRuntimeKind profile, AiProjectDebugOperation operation) const noexcept;

    private:
        friend Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &);

        ProjectAiSettings(AI::AiTaskSchedulerSettings scheduler, bool mutation, bool capture)
            : scheduler_(std::move(scheduler)), mutation_(mutation), capture_(capture) {}

        AI::AiTaskSchedulerSettings scheduler_;
        bool mutation_{};
        bool capture_{};
    };

    /** @brief Resolves versioned settings.ai from the canonical project source without creating a Scene.
     * @param source Strictly parsed project source document.
     * @return Validated authored policy, absence for older projects, or a typed policy diagnostic.
     */
    [[nodiscard]] Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &source);
}  // namespace Horo::Application
