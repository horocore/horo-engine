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
                code == Source::DiskChanged.code.Value() || code == Source::Removed.code.Value() || code == Source::Closed.code.Value())
                return Feedback::Conflict;
            return fallback;
        }
    }  // namespace

    /** @copydoc QueueSourceReviewApplication */
    bool QueueSourceReviewApplication(const std::string_view project, const Proposals::ReviewDecisions &decisions,
                                      const Proposals::ProjectPolicy &policy, const Proposals::WorkspaceEditPlan &plan,
                                      const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::None || policy.Project() != project || lifetime.IsCancellationRequested())
            return false;
        command.sourceReviewApplication = std::make_shared<const SourceReviewApplication>(SourceReviewApplication{decisions, policy, plan});
        command.sourceReviewLifetime = lifetime;
        command.command = EditorWorkspaceViewCommand::ApplySourceReview;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Status */
    Proposals::SourceTransactionStatus SourceReviewTransactions::Status() const noexcept {
        const bool canUndo = batch_.CanUndo() && std::ranges::all_of(batch_.Items(), [](const auto &item) {
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
    bool SourceReviewTransactions::Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents) {
        switch (command.command) {
            case EditorWorkspaceViewCommand::ApplySourceReview:
                Apply(command.sourceReviewApplication.get(), documents, command.sourceReviewLifetime);
                return true;
            case EditorWorkspaceViewCommand::UndoSourceReview:
                Undo(command.sourceReviewUndo, documents, command.sourceReviewLifetime);
                return true;
            default:
                return false;
        }
    }

    /** @copydoc SourceReviewTransactions::Apply */
    void SourceReviewTransactions::Apply(const SourceReviewApplication *request, SourceDocumentService &documents,
                                         const CancellationToken &lifetime) {
        feedback_ = lifetime.IsCancellationRequested() ? Feedback::Cancelled : Feedback::ApplyFailed;
        if (feedback_ == Feedback::Cancelled)
            return;
        if (!request)
            return;
        auto applied = request->decisions.ApplyAccepted(documents, request->policy, request->plan, lifetime);
        if (applied.HasError()) {
            feedback_ = FailureFeedback(applied.ErrorValue(), Feedback::ApplyFailed);
            return;
        }
        batch_ = std::move(applied).Value();
        feedback_ = Feedback::Applied;
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
