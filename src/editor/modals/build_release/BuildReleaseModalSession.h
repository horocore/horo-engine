#pragma once

/** @file BuildReleaseModalSession.h
 * @brief Service-backed, presentation-independent state for the Build & Release modal.
 */

#include "Horo/Release/ReleaseJobTracker.h"
#include "Horo/Release/ReleasePreflight.h"

#include <optional>
#include <vector>

namespace Horo::Editor {
    /** @brief Narrow release capability borrowed by a modal; the host retains job ownership. */
    class IBuildReleaseJobs {
    public:
        virtual ~IBuildReleaseJobs() = default;
        [[nodiscard]] virtual std::vector<Release::ReleaseJobSnapshot> List() const = 0;
        [[nodiscard]] virtual std::optional<Release::ReleaseDiagnostic> Diagnostic(Release::ReleaseJobId job,
                                                                                   Release::ReleaseDiagnosticId diagnostic) const = 0;
        [[nodiscard]] virtual Result<Release::ReleaseJobId> Submit(Release::ReleaseExecutionPlan plan) = 0;
        [[nodiscard]] virtual Result<void> RequestCancel(Release::ReleaseJobId job) = 0;
    };

    /** @brief Reconnects the modal to retained service jobs without changing their lifetime. */
    class BuildReleaseModalSession final {
    public:
        explicit BuildReleaseModalSession(IBuildReleaseJobs &jobs) noexcept;

        /** @brief Loads active and recent jobs, preferring the newest active job. */
        void Open();
        /** @brief Refreshes immutable snapshots while preserving a valid selection. */
        void Refresh();
        /** @brief Selects a retained job. @return Whether that job is still known. */
        [[nodiscard]] bool Select(Release::ReleaseJobId job) noexcept;
        /** @brief Submits only when explicitly invoked; no open or refresh path calls this. */
        [[nodiscard]] Result<Release::ReleaseJobId> Submit(Release::ReleaseExecutionPlan plan);
        /** @brief Explicitly requests cancellation of the selected active job. */
        [[nodiscard]] Result<void> RequestCancel();

        [[nodiscard]] const std::vector<Release::ReleaseJobSnapshot> &Jobs() const noexcept;
        [[nodiscard]] const Release::ReleaseJobSnapshot *Selected() const noexcept;
        [[nodiscard]] std::vector<Release::ReleaseDiagnostic> SelectedDiagnostics() const;
        /** @brief A finalized or pre-sign-verified payload is never reported as final verified. */
        [[nodiscard]] bool SelectedCandidateIsFinalVerified() const noexcept;

    private:
        [[nodiscard]] const Release::ReleaseJobSnapshot *Find(Release::ReleaseJobId job) const noexcept;
        IBuildReleaseJobs &jobs_;
        std::vector<Release::ReleaseJobSnapshot> snapshots_;
        std::optional<Release::ReleaseJobId> selected_;
    };
}  // namespace Horo::Editor
