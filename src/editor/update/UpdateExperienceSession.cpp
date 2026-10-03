#include "editor/update/UpdateExperienceSession.h"

#include <mutex>
#include <utility>

namespace Horo::Editor {
    struct UpdateExperienceSession::Completion final {
        std::mutex mutex;
        std::optional<EditorUpdateOffer> offer;
        bool checkedUpToDate{};
        std::optional<Error> failure;
        EditorUpdatePhase progressPhase{EditorUpdatePhase::Downloading};
        std::uint64_t transferredBytes{};
        std::uint64_t totalBytes{};
    };

    UpdateExperienceSession::UpdateExperienceSession(JobSystem &jobs, IEditorUpdateBackend &backend)
        : jobs_(jobs), backend_(backend), completion_(std::make_shared<Completion>()) {}

    UpdateExperienceSession::~UpdateExperienceSession() {
        Shutdown();
    }

    const EditorUpdateSnapshot &UpdateExperienceSession::Snapshot() const noexcept {
        return snapshot_;
    }

    bool UpdateExperienceSession::SetChannel(EditorUpdateChannel channel, const bool confirmed) {
        using enum EditorUpdateChannelKind;
        if (shutdown_ || job_)
            return false;
        if (snapshot_.channel == channel)
            return true;
        if (!confirmed && (channel.kind == Preview || channel.kind == Nightly))
            return false;
        snapshot_.channel = std::move(channel);
        snapshot_.offline = snapshot_.channel.kind == Offline;
        snapshot_.offer.reset();
        snapshot_.phase = EditorUpdatePhase::Idle;
        snapshot_.diagnostic.clear();
        return true;
    }

    void UpdateExperienceSession::SetAutomaticDownload(const bool enabled) noexcept {
        snapshot_.automaticDownload = enabled;
    }

    void UpdateExperienceSession::SetInstallOnExit(const bool enabled) noexcept {
        snapshot_.installOnExit = enabled;
    }

    bool UpdateExperienceSession::CheckNow() {
        return Start(Operation::Check);
    }

    bool UpdateExperienceSession::Download() {
        return snapshot_.offer && snapshot_.phase == EditorUpdatePhase::Available && Start(Operation::Prepare);
    }

    bool UpdateExperienceSession::RestartNow(const bool confirmed) {
        return confirmed && snapshot_.phase == EditorUpdatePhase::RestartRequired && Start(Operation::Activate);
    }

    Result<bool> UpdateExperienceSession::ActivateOnExit() {
        using enum EditorUpdatePhase;
        if (shutdown_ || job_ || !snapshot_.installOnExit || snapshot_.phase != RestartRequired)
            return Result<bool>::Success(false);
        if (const Result<void> requested = backend_.Activate({}); requested.HasError()) {
            snapshot_.phase = Failed;
            snapshot_.diagnostic = requested.ErrorValue().message;
            return Result<bool>::Failure(requested.ErrorValue());
        }
        snapshot_.phase = Activating;
        return Result<bool>::Success(true);
    }

    bool UpdateExperienceSession::Rollback(const bool confirmed) {
        return confirmed && (snapshot_.phase == EditorUpdatePhase::Active || snapshot_.phase == EditorUpdatePhase::Failed) &&
               Start(Operation::Rollback);
    }

    bool UpdateExperienceSession::Cancel() {
        if (!job_ || !snapshot_.canCancel)
            return false;
        cancellation_.RequestCancellation();
        static_cast<void>(job_->RequestCancel());
        return true;
    }

    bool UpdateExperienceSession::ReportVerifiedHostOutcome(const EditorUpdatePhase outcome) {
        using enum EditorUpdatePhase;
        if (shutdown_ || job_ || (outcome != Active && outcome != RolledBack))
            return false;
        if (const EditorUpdatePhase pending = outcome == Active ? Activating : RollbackPending;
            snapshot_.phase != pending && snapshot_.phase != Idle)
            return false;
        snapshot_.phase = outcome;
        snapshot_.diagnostic.clear();
        return true;
    }

    bool UpdateExperienceSession::Start(const Operation operation) {
        if (shutdown_ || job_ || operation == Operation::None)
            return false;
        cancellation_ = CancellationSource{};
        auto completion = std::make_shared<Completion>();
        const auto channel = snapshot_.channel;
        const auto offer = snapshot_.offer;
        auto *backend = &backend_;
        auto submitted = jobs_.SubmitResult({.parentCancellation = cancellation_.Token()},
                                            [backend, completion, operation, channel, offer](const CancellationToken &cancellation) {
            return RunOperation(*backend, completion, operation, channel, offer, cancellation);
        });
        if (submitted.HasError()) {
            snapshot_.phase = EditorUpdatePhase::Failed;
            snapshot_.diagnostic = submitted.ErrorValue().message;
            return false;
        }
        completion_ = std::move(completion);
        operation_ = operation;
        job_ = std::move(submitted).Value();
        if (operation == Operation::Check)
            snapshot_.phase = EditorUpdatePhase::Checking;
        else if (operation == Operation::Prepare)
            snapshot_.phase = EditorUpdatePhase::Downloading;
        else if (operation == Operation::Activate)
            snapshot_.phase = EditorUpdatePhase::Activating;
        else
            snapshot_.phase = EditorUpdatePhase::RollbackPending;
        snapshot_.canCancel = operation == Operation::Check || operation == Operation::Prepare;
        snapshot_.diagnostic.clear();
        snapshot_.transferredBytes = 0U;
        snapshot_.totalBytes = 0U;
        return true;
    }

    Result<void> UpdateExperienceSession::RunOperation(IEditorUpdateBackend &backend, const std::shared_ptr<Completion> &completion,
                                                       const Operation operation, const EditorUpdateChannel &channel,
                                                       const std::optional<EditorUpdateOffer> &offer,
                                                       const CancellationToken cancellation) {
        Result<void> result = Result<void>::Success();
        if (operation == Operation::Check) {
            auto checked = backend.Check(channel, cancellation);
            std::lock_guard lock(completion->mutex);
            if (checked.HasError()) {
                completion->failure = checked.ErrorValue();
                result = Result<void>::Failure(checked.ErrorValue());
            } else if (checked.Value()) {
                completion->offer = *checked.Value();
            } else {
                completion->checkedUpToDate = true;
            }
        } else if (operation == Operation::Prepare) {
            result =
                backend.Prepare(*offer, cancellation,
                                [completion](const EditorUpdatePhase phase, const std::uint64_t transferred, const std::uint64_t total) {
                std::lock_guard lock(completion->mutex);
                completion->progressPhase = phase;
                completion->transferredBytes = transferred;
                completion->totalBytes = total;
            });
        } else if (operation == Operation::Activate) {
            result = backend.Activate(cancellation);
        } else if (operation == Operation::Rollback) {
            result = backend.Rollback(cancellation);
        }
        if (result.HasError()) {
            std::lock_guard lock(completion->mutex);
            completion->failure = result.ErrorValue();
        }
        return result;
    }

    void UpdateExperienceSession::Poll() {
        if (!job_) {
            if (snapshot_.automaticDownload && snapshot_.phase == EditorUpdatePhase::Available)
                static_cast<void>(Download());
            return;
        }
        const auto jobSnapshot = job_->Snapshot();
        if (!jobSnapshot)
            return;
        std::lock_guard lock(completion_->mutex);
        if (operation_ == Operation::Prepare) {
            snapshot_.phase = completion_->progressPhase;
            snapshot_.transferredBytes = completion_->transferredBytes;
            snapshot_.totalBytes = completion_->totalBytes;
        }
        if (jobSnapshot->state == JobState::Queued || jobSnapshot->state == JobState::Running)
            return;
        snapshot_.canCancel = false;
        if (jobSnapshot->state == JobState::Cancelled) {
            snapshot_.phase = operation_ == Operation::Check ? EditorUpdatePhase::Idle : EditorUpdatePhase::Available;
            snapshot_.diagnostic.clear();
        } else if (jobSnapshot->state == JobState::Failed || completion_->failure) {
            snapshot_.phase = EditorUpdatePhase::Failed;
            snapshot_.diagnostic = completion_->failure ? completion_->failure->message : "Update operation failed";
        } else if (operation_ == Operation::Check) {
            snapshot_.offer = completion_->offer;
            snapshot_.phase = completion_->checkedUpToDate ? EditorUpdatePhase::UpToDate : EditorUpdatePhase::Available;
        } else if (operation_ == Operation::Prepare) {
            snapshot_.phase =
                snapshot_.offer && snapshot_.offer->requiresRestart ? EditorUpdatePhase::RestartRequired : EditorUpdatePhase::Staged;
        } else if (operation_ == Operation::Activate) {
            snapshot_.phase = EditorUpdatePhase::Activating;
        } else if (operation_ == Operation::Rollback) {
            snapshot_.phase = EditorUpdatePhase::RollbackPending;
        }
        job_.reset();
        operation_ = Operation::None;
    }

    void UpdateExperienceSession::Shutdown() noexcept {
        if (shutdown_)
            return;
        shutdown_ = true;
        if (job_) {
            cancellation_.RequestCancellation();
            static_cast<void>(job_->RequestCancel());
            static_cast<void>(job_->Wait());
            job_.reset();
        }
    }
}  // namespace Horo::Editor
