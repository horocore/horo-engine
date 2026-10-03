#include "editor/modals/build_release/BuildReleaseModalSession.h"

#include "Horo/Foundation/ErrorCode.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool IsActive(const Release::ReleaseJobState state) noexcept {
            using enum Release::ReleaseJobState;
            return state == Queued || state == Running || state == Cancelling;
        }

        [[nodiscard]] Error NoActiveJob() {
            return Error{ErrorCode{"editor.release.no_active_job"}, ErrorDomainId{"horo.editor"}};
        }
    }  // namespace

    BuildReleaseModalSession::BuildReleaseModalSession(IBuildReleaseJobs &jobs) noexcept : jobs_(jobs) {}

    void BuildReleaseModalSession::Open() {
        selected_.reset();
        Refresh();
    }

    void BuildReleaseModalSession::Refresh() {
        snapshots_ = jobs_.List();
        if (selected_ && Find(*selected_) != nullptr)
            return;
        selected_.reset();
        for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
            if (IsActive(it->state)) {
                selected_ = it->id;
                return;
            }
        }
        if (!snapshots_.empty())
            selected_ = snapshots_.back().id;
    }

    bool BuildReleaseModalSession::Select(const Release::ReleaseJobId job) noexcept {
        if (Find(job) == nullptr)
            return false;
        selected_ = job;
        return true;
    }

    Result<Release::ReleaseJobId> BuildReleaseModalSession::Submit(Release::ReleaseExecutionPlan plan) {
        auto submitted = jobs_.Submit(std::move(plan));
        if (submitted.HasValue()) {
            const Release::ReleaseJobId job = submitted.Value();
            Refresh();
            static_cast<void>(Select(job));
        }
        return submitted;
    }

    Result<void> BuildReleaseModalSession::RequestCancel() {
        const auto *snapshot = Selected();
        if (snapshot == nullptr || !IsActive(snapshot->state))
            return Result<void>::Failure(NoActiveJob());
        return jobs_.RequestCancel(snapshot->id);
    }

    const std::vector<Release::ReleaseJobSnapshot> &BuildReleaseModalSession::Jobs() const noexcept {
        return snapshots_;
    }

    const Release::ReleaseJobSnapshot *BuildReleaseModalSession::Selected() const noexcept {
        return selected_ ? Find(*selected_) : nullptr;
    }

    std::vector<Release::ReleaseDiagnostic> BuildReleaseModalSession::SelectedDiagnostics() const {
        std::vector<Release::ReleaseDiagnostic> diagnostics;
        const auto *snapshot = Selected();
        if (snapshot == nullptr)
            return diagnostics;
        diagnostics.reserve(snapshot->recentDiagnostics.size());
        for (const auto id : snapshot->recentDiagnostics) {
            if (auto diagnostic = jobs_.Diagnostic(snapshot->id, id))
                diagnostics.push_back(std::move(*diagnostic));
        }
        return diagnostics;
    }

    bool BuildReleaseModalSession::SelectedCandidateIsFinalVerified() const noexcept {
        const auto *snapshot = Selected();
        if (snapshot == nullptr || !snapshot->candidate || snapshot->candidate->state != Release::ReleaseCandidateState::FinalVerified)
            return false;
        const auto index = static_cast<std::size_t>(Release::ReleaseStage::FinalVerifying);
        return snapshot->stages[index].state == Release::ReleaseStageState::Succeeded;
    }

    const Release::ReleaseJobSnapshot *BuildReleaseModalSession::Find(const Release::ReleaseJobId job) const noexcept {
        const auto found = std::ranges::find(snapshots_, job, &Release::ReleaseJobSnapshot::id);
        return found == snapshots_.end() ? nullptr : std::to_address(found);
    }
}  // namespace Horo::Editor
