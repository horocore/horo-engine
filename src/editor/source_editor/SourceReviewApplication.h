#pragma once

/** @file SourceReviewApplication.h @brief Deferred, explicitly requested application of a reviewed proposal. */
#include "Horo/Editor/ProposedChangeSet.h"

namespace Horo::Editor {
    struct EditorWorkspaceViewCommandData;

    /** @brief Owns the exact review decisions and evidence until the workspace safely dispatches the command. */
    struct SourceReviewApplication final {
        Proposals::ReviewDecisions decisions;
        Proposals::ProjectPolicy policy;
        Proposals::WorkspaceEditPlan plan;
    };

    /** @brief Queues an exact source review only within the current workspace and an idle command slot. */
    [[nodiscard]] bool QueueSourceReviewApplication(std::string_view project, const Proposals::ReviewDecisions &decisions,
                                                    const Proposals::ProjectPolicy &policy, const Proposals::WorkspaceEditPlan &plan,
                                                    EditorWorkspaceViewCommandData &command);

    /** @brief Owns the last applied source group and dispatches explicit editor source transaction commands. */
    class SourceReviewTransactions final {
    public:
        /** @brief Consumes Apply or Undo through the source document owner. @return True for a recognized command. */
        bool Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents);
        /** @brief Reads the latest feedback and whether every changed document still permits group undo. */
        [[nodiscard]] Proposals::SourceTransactionStatus Status() const noexcept;
        /** @brief Stages one grouped undo when its receipt remains current. */
        [[nodiscard]] bool QueueUndo(EditorWorkspaceViewCommandData &command) const;

    private:
        /** @brief Applies a complete accepted subset and captures its immutable receipt. */
        void Apply(const SourceReviewApplication *request, SourceDocumentService &documents);
        /** @brief Replays the current source review as one group. */
        void Undo(const SourceEditBatch &receipt, SourceDocumentService &documents);
        SourceEditBatch batch_;
        Proposals::SourceTransactionFeedback feedback_{};
    };
}  // namespace Horo::Editor
