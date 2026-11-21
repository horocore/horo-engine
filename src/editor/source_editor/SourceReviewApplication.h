#pragma once

/** @file SourceReviewApplication.h @brief Deferred, explicitly requested application of a reviewed proposal. */
#include "Horo/Editor/ProposedChangeSet.h"
#include "Horo/Foundation/JobSystem.h"

namespace Horo::Editor {
    struct EditorWorkspaceViewCommandData;
    class ProjectMutationCoordinator;

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
        /** @brief Consumes Apply or Undo through the source document owner.
         * @param command Deferred owner-lane request. @param documents Live source owner.
         * @param jobs Host job system. @param root Authorized workspace root.
         * @param files Host filesystem authority, retained by the host until its job system drains.
         * @return True for a recognized command.
         */
        bool Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents, const JobSystem *jobs,
                     const std::filesystem::path &root, const DurableFileSystem *files);
        /** @brief Polls one preparing job without waiting. @return True if a buffer group was published. */
        bool Poll(SourceDocumentService &documents, const DurableFileSystem *files);
        /** @brief Polls history jobs and requests the initial read once for this workspace.
         * @param jobs Host job system. @param root Authorized project root.
         * @details Adopted attribution carries no source owner receipt or undo authority.
         */
        void UpdateAuditHistory(const JobSystem *jobs, const std::filesystem::path &root);
        /** @brief Records the just-published receipt after Poll returned true.
         * @param jobs Host job system. @param root Authorized project root.
         * @param mutations Shared project mutation owner. @param files Tracked filesystem.
         * @pre Host owners outlive their jobs. No source bytes cross the history boundary.
         */
        void RecordAppliedAudit(const JobSystem *jobs, const std::filesystem::path &root, const ProjectMutationCoordinator *mutations,
                                DurableFileSystem *files);
        /** @brief Reads the latest feedback and whether every changed document still permits group undo. */
        [[nodiscard]] Proposals::SourceTransactionStatus Status() const noexcept;
        /** @brief Stages one grouped undo when its receipt remains current. */
        [[nodiscard]] bool QueueUndo(const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) const;

    private:
        /** @brief Submits immutable review preparation on the host job system. */
        void Start(std::shared_ptr<const SourceReviewApplication> request, const JobSystem *jobs, const std::filesystem::path &root,
                   const DurableFileSystem *files, const CancellationToken &lifetime);
        /** @brief Replays the current source review as one group. */
        void Undo(const SourceEditBatch &receipt, SourceDocumentService &documents, const CancellationToken &lifetime);
        SourceEditBatch batch_;
        std::shared_ptr<const Proposals::SourceReviewAudit> audit_;
        Proposals::SourceTransactionFeedback feedback_{};

        struct Preparation final {
            JobHandle job;
            CancellationToken lifetime;
            std::shared_ptr<std::optional<Proposals::PreparedSourceReview>> output;
        };

        std::optional<Preparation> preparing_;

        struct HistoryRead final {
            JobHandle job;
            std::shared_ptr<std::shared_ptr<const Proposals::SourceReviewAudit>> latest;
        };

        std::optional<HistoryRead> auditRead_;
        std::optional<JobHandle> auditWrite_;
        bool auditReadRequested_{};
        bool auditHistoryError_{};
    };
}  // namespace Horo::Editor
