#pragma once

/** @file SourceReviewApplication.h @brief Deferred, explicitly requested application of a reviewed proposal. */
#include "Horo/Editor/ProposedChangeSet.h"
#include "Horo/Foundation/JobSystem.h"

namespace Horo::Editor {
    struct EditorWorkspaceViewCommandData;

    /** @brief Owns the exact review decisions and evidence until the workspace safely dispatches the command. */
    struct SourceReviewApplication final {
        Proposals::ReviewDecisions decisions;
        Proposals::ProjectPolicy policy;
        std::shared_ptr<const Proposals::WorkspaceEditPlan> plan;
    };

    /** @brief Queues an exact source review only within the current workspace and an idle command slot. */
    [[nodiscard]] bool QueueSourceReviewApplication(std::string_view project, const Proposals::ReviewDecisions &decisions,
                                                    const Proposals::ProjectPolicy &policy,
                                                    const std::shared_ptr<const Proposals::WorkspaceEditPlan> &plan,
                                                    const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command);

    /** @brief Owns the last applied source group and dispatches explicit editor source transaction commands. */
    class SourceReviewTransactions final {
    public:
        /** @brief Consumes Apply or Undo through the source document owner. @return True for a recognized command. */
        bool Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents, const JobSystem *jobs);
        /** @brief Polls one preparing job without waiting. @return True if a buffer group was published. */
        bool Poll(SourceDocumentService &documents);
        /** @brief Reads the latest feedback and whether every changed document still permits group undo. */
        [[nodiscard]] Proposals::SourceTransactionStatus Status() const noexcept;
        /** @brief Stages one grouped undo when its receipt remains current. */
        [[nodiscard]] bool QueueUndo(const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) const;

    private:
        /** @brief Submits immutable review preparation on the host job system. */
        void Start(std::shared_ptr<const SourceReviewApplication> request, const JobSystem *jobs, const CancellationToken &lifetime);
        /** @brief Replays the current source review as one group. */
        void Undo(const SourceEditBatch &receipt, SourceDocumentService &documents, const CancellationToken &lifetime);
        SourceEditBatch batch_;
        Proposals::SourceTransactionFeedback feedback_{};

        struct Preparation final {
            JobHandle job;
            CancellationToken lifetime;
            std::shared_ptr<std::optional<Proposals::PreparedSourceReview>> output;
        };

        std::optional<Preparation> preparing_;
    };
}  // namespace Horo::Editor
