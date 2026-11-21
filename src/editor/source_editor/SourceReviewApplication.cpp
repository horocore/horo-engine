#include "SourceReviewApplication.h"

#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        using Feedback = Proposals::SourceTransactionFeedback;

        /** @brief Distinguishes lost source or workspace evidence from an invalid operation. */
        Feedback FailureFeedback(const Error &error, const Feedback fallback) {
            namespace Source = SourceDocumentErrors;
            const auto code = error.code.Value();
            if (code == Source::Cancelled.code.Value())
                return Feedback::Cancelled;
            if (code == Proposals::Errors::StaleBase.code.Value() || code == Proposals::Errors::PolicyDenied.code.Value() ||
                code == Source::Stale.code.Value() || code == Source::SaveConflict.code.Value() ||
                code == Source::DiskChanged.code.Value() || code == Source::Removed.code.Value() || code == Source::Closed.code.Value() ||
                code == Source::ReadFailed.code.Value())
                return Feedback::Conflict;
            return fallback;
        }
    }  // namespace

    /** @copydoc QueueSourceReviewApplication */
    bool QueueSourceReviewApplication(const std::string_view project, const Proposals::ReviewDecisions &decisions,
                                      const Proposals::ProjectPolicy &policy,
                                      const std::shared_ptr<const Proposals::WorkspaceEditPlan> &plan, const CancellationToken &lifetime,
                                      EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::None || !plan || policy.Project() != project ||
            lifetime.IsCancellationRequested())
            return false;
        command.sourceReviewApplication = std::make_shared<const SourceReviewApplication>(SourceReviewApplication{decisions, policy, plan});
        command.sourceReviewLifetime = lifetime;
        command.command = EditorWorkspaceViewCommand::ApplySourceReview;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Status */
    Proposals::SourceTransactionStatus SourceReviewTransactions::Status() const noexcept {
        const bool canUndo = !preparing_ && batch_.CanUndo() && std::ranges::all_of(batch_.Items(), [](const auto &item) {
            return !item.changed || item.snapshot.IsCurrent();
        });
        return {feedback_, canUndo};
    }

    /** @copydoc SourceReviewTransactions::QueueUndo */
    bool SourceReviewTransactions::QueueUndo(const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) const {
        if (command.command != EditorWorkspaceViewCommand::None || !Status().canUndo || lifetime.IsCancellationRequested())
            return false;
        command.sourceReviewUndo = batch_;
        command.sourceReviewLifetime = lifetime;
        command.command = EditorWorkspaceViewCommand::UndoSourceReview;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Process */
    bool SourceReviewTransactions::Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents,
                                           const JobSystem *jobs, const std::filesystem::path &root, const DurableFileSystem *files) {
        switch (command.command) {
            case EditorWorkspaceViewCommand::ApplySourceReview:
                Start(command.sourceReviewApplication, jobs, root, files, command.sourceReviewLifetime);
                return true;
            case EditorWorkspaceViewCommand::UndoSourceReview:
                if (!preparing_)
                    Undo(command.sourceReviewUndo, documents, command.sourceReviewLifetime);
                return true;
            default:
                return false;
        }
    }

    /** @copydoc SourceReviewTransactions::Start */
    void SourceReviewTransactions::Start(std::shared_ptr<const SourceReviewApplication> request, const JobSystem *jobs,
                                         const std::filesystem::path &root, const DurableFileSystem *files,
                                         const CancellationToken &lifetime) {
        if (preparing_)
            return;
        feedback_ = lifetime.IsCancellationRequested() ? Feedback::Cancelled : Feedback::ApplyFailed;
        if (feedback_ == Feedback::Cancelled || !jobs || !files || !request || !request->plan)
            return;
        try {
            auto output = std::make_shared<std::optional<Proposals::PreparedSourceReview>>();
            auto job =
                jobs->SubmitResult({.parentCancellation = lifetime, .priority = JobPriority::Interactive, .resource = JobResource::Io},
                                   [request = std::move(request), output, root, files](const CancellationToken &stop) {
                auto prepared = request->decisions.PrepareAccepted(request->policy, *request->plan, stop);
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                auto candidate = std::move(prepared).Value();
                if (const auto disk = candidate.ObserveDisk(root, *files, stop); disk.HasError())
                    return Result<void>::Failure(disk.ErrorValue());
                output->emplace(std::move(candidate));
                return Result<void>::Success();
            });
            if (job.HasValue()) {
                preparing_.emplace(Preparation{std::move(job).Value(), lifetime, std::move(output)});
                feedback_ = Feedback::Preparing;
            }
        } catch (const std::bad_alloc &) {
            // No owner mutation or receipt publication occurs on admission failure.
        }
    }

    /** @copydoc SourceReviewTransactions::Poll */
    bool SourceReviewTransactions::Poll(SourceDocumentService &documents, const DurableFileSystem *files) {
        if (!preparing_)
            return false;
        const auto job = preparing_->job.Snapshot();
        if (!job || !job->terminalResult)
            return false;
        auto preparation = std::move(*preparing_);
        preparing_.reset();
        if (preparation.lifetime.IsCancellationRequested() || job->state == JobState::Cancelled) {
            feedback_ = Feedback::Cancelled;
            return false;
        }
        if (const auto &error = job->terminalResult->error; error || !*preparation.output || !files) {
            feedback_ = error ? FailureFeedback(*error, Feedback::ApplyFailed) : Feedback::ApplyFailed;
            return false;
        }
        auto applied = preparation.output->value().CommitObserved(documents, *files, preparation.lifetime);
        if (applied.HasError()) {
            feedback_ = FailureFeedback(applied.ErrorValue(), Feedback::ApplyFailed);
            return false;
        }
        batch_ = std::move(applied).Value();
        feedback_ = Feedback::Applied;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Undo */
    void SourceReviewTransactions::Undo(const SourceEditBatch &receipt, SourceDocumentService &documents,
                                        const CancellationToken &lifetime) {
        if (lifetime.IsCancellationRequested()) {
            feedback_ = Feedback::Cancelled;
            return;
        }
        feedback_ = Feedback::Conflict;
        if (!batch_.SharesStateWith(receipt))
            return;
        const auto undone = documents.UndoBatch(batch_, lifetime);
        if (undone.HasError()) {
            feedback_ = FailureFeedback(undone.ErrorValue(), Feedback::UndoFailed);
            return;
        }
        batch_ = {};
        feedback_ = Feedback::Undone;
    }
}  // namespace Horo::Editor
