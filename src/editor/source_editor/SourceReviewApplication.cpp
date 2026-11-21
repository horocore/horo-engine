#include "SourceReviewApplication.h"

#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc QueueSourceReviewApplication */
    bool QueueSourceReviewApplication(const std::string_view project, const Proposals::ReviewDecisions &decisions,
                                      const Proposals::ProjectPolicy &policy, const Proposals::WorkspaceEditPlan &plan,
                                      EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::None || policy.Project() != project)
            return false;
        command.sourceReviewApplication = std::make_shared<const SourceReviewApplication>(SourceReviewApplication{decisions, policy, plan});
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
    bool SourceReviewTransactions::QueueUndo(EditorWorkspaceViewCommandData &command) const {
        if (command.command != EditorWorkspaceViewCommand::None || !Status().canUndo)
            return false;
        command.sourceReviewUndo = batch_;
        command.command = EditorWorkspaceViewCommand::UndoSourceReview;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Process */
    bool SourceReviewTransactions::Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents) {
        switch (command.command) {
            case EditorWorkspaceViewCommand::ApplySourceReview:
                Apply(command.sourceReviewApplication.get(), documents);
                return true;
            case EditorWorkspaceViewCommand::UndoSourceReview:
                Undo(command.sourceReviewUndo, documents);
                return true;
            default:
                return false;
        }
    }

    /** @copydoc SourceReviewTransactions::Apply */
    void SourceReviewTransactions::Apply(const SourceReviewApplication *request, SourceDocumentService &documents) {
        feedback_ = Proposals::SourceTransactionFeedback::ApplyFailed;
        if (!request)
            return;
        auto applied = request->decisions.ApplyAccepted(documents, request->policy, request->plan);
        if (applied.HasError())
            return;
        batch_ = std::move(applied).Value();
        feedback_ = Proposals::SourceTransactionFeedback::Applied;
    }

    /** @copydoc SourceReviewTransactions::Undo */
    void SourceReviewTransactions::Undo(const SourceEditBatch &receipt, SourceDocumentService &documents) {
        feedback_ = Proposals::SourceTransactionFeedback::UndoFailed;
        if (!batch_.SharesStateWith(receipt) || documents.UndoBatch(batch_).HasError())
            return;
        batch_ = {};
        feedback_ = Proposals::SourceTransactionFeedback::Undone;
    }
}  // namespace Horo::Editor
