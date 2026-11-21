#pragma once

/** @file SourceReviewApplication.h @brief Deferred, explicitly requested application of a reviewed proposal. */
#include "Horo/Editor/ProposedChangeSet.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"

namespace Horo::Editor {
    struct EditorWorkspaceViewCommandData;
    class ProjectMutationCoordinator;
    class SourceFileOpenService;
    class WorkspacePanelHost;

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
         * @param open Source and registry owner for staged file operations.
         * @param mutations Project mutation authority for restoring a deleted source.
         * @return True for a recognized command.
         */
        bool Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents, const JobSystem *jobs,
                     const std::filesystem::path &root, DurableFileSystem *files, SourceFileOpenService *open = nullptr,
                     const ProjectMutationCoordinator *mutations = nullptr);
        /** @brief Polls one preparing job without waiting.
         * @param documents Current source owner. @param files Host filesystem authority.
         * @param open Source and registry owner for staged file operations.
         * @param host Workspace presentation using the same registry, when present.
         * @param mutations Project mutation authority for explicit file moves.
         * @return True if a source draft or buffer group was published.
         */
        bool Poll(SourceDocumentService &documents, DurableFileSystem *files, SourceFileOpenService *open = nullptr,
                  WorkspacePanelHost *host = nullptr, const ProjectMutationCoordinator *mutations = nullptr);
        /** @brief Polls history jobs and requests the initial read once for this workspace.
         * @param jobs Host job system. @param root Authorized project root.
         * @param mutations Shared project mutation owner. @param files Tracked filesystem.
         * @details Adopted attribution carries no source owner receipt or undo authority.
         */
        void UpdateAuditHistory(const JobSystem *jobs, const std::filesystem::path &root,
                                const ProjectMutationCoordinator *mutations = nullptr, DurableFileSystem *files = nullptr);
        /** @brief Records the just-published receipt after Poll returned true.
         * @param jobs Host job system. @param root Authorized project root.
         * @param mutations Shared project mutation owner. @param files Tracked filesystem.
         * @pre Host owners outlive their jobs. No source bytes cross the history boundary.
         */
        void RecordAppliedAudit(const JobSystem *jobs, const std::filesystem::path &root, const ProjectMutationCoordinator *mutations,
                                DurableFileSystem *files);
        /** @brief Drains an application write and its final observed outcome during orderly workspace retirement.
         * @param jobs Host job system. @param root Authorized project root.
         * @param mutations Shared project mutation owner. @param files Tracked filesystem.
         * @return True only if the queued history writes completed durably within the shutdown wait.
         */
        [[nodiscard]] bool DrainAuditHistory(const JobSystem *jobs, const std::filesystem::path &root,
                                             const ProjectMutationCoordinator *mutations, DurableFileSystem *files);
        /** @brief Reads the latest feedback and whether every changed document still permits group undo. */
        [[nodiscard]] Proposals::SourceTransactionStatus Status() const noexcept;
        /** @brief Stages one grouped undo when its receipt remains current. */
        [[nodiscard]] bool QueueUndo(const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) const;

    private:
        /** @brief Classifies the latest successful group's owner or save evidence. */
        [[nodiscard]] Proposals::SourceReviewDisposition Disposition() const noexcept;
        /** @brief Schedules the initial read only when no earlier read was submitted or attempted. */
        void BeginHistoryRead(const JobSystem *jobs, const std::filesystem::path &root);
        /** @brief Consumes one completed write without waiting on an I/O worker. */
        void PollHistoryWrite();

        /** @brief Submits immutable review preparation on the host job system. */
        void Start(std::shared_ptr<const SourceReviewApplication> request, const JobSystem *jobs, const std::filesystem::path &root,
                   const DurableFileSystem *files, const CancellationToken &lifetime);
        /** @brief Replays the current source review as one group. */
        void Undo(const SourceEditBatch &receipt, SourceDocumentService &documents, const CancellationToken &lifetime);
        /** @brief Directs an exact unchanged draft undo through the owning document registry. */
        void UndoDraft(const SourceDocumentSnapshot &receipt, SourceFileOpenService *open, const CancellationToken &lifetime);
        /** @brief Reverts the exact edit and draft receipt together. */
        void UndoDraftGroup(const std::shared_ptr<const SourceDraftBatch> &receipt, SourceFileOpenService *open,
                            const CancellationToken &lifetime);
        /** @brief Restores the current review's owned Trash payload without overwriting an existing file. */
        void UndoDelete(const SourceDocumentSnapshot &receipt, SourceFileOpenService *open, DurableFileSystem *files,
                        const ProjectMutationCoordinator *mutations, const CancellationToken &lifetime);
        /** @brief Observes the exact current renamed file before restoring its original name and buffer. */
        void StartRenameRestore(const SourceDocumentSnapshot &receipt, const JobSystem *jobs, const std::filesystem::path &root,
                                const DurableFileSystem *files, const CancellationToken &lifetime);
        /** @brief Stages and presents one draft or retires the draft if the workspace rejects its tab. */
        bool CommitDraft(Proposals::PreparedSourceCreate &creation, SourceFileOpenService *open, WorkspacePanelHost *host,
                         const CancellationToken &lifetime);
        /** @brief Moves an explicitly confirmed clean source to project Trash before closing its tab. */
        bool CommitDelete(Proposals::PreparedSourceDelete &deletion, SourceFileOpenService *open, DurableFileSystem &files,
                          const ProjectMutationCoordinator *mutations, const CancellationToken &lifetime);
        /** @brief Applies the confirmed rename through the source owner and its routing registry. */
        bool CommitRename(Proposals::PreparedSourceRename &rename, SourceFileOpenService *open, DurableFileSystem &files,
                          const ProjectMutationCoordinator *mutations, const CancellationToken &lifetime);

        struct RenameRestore final {
            SourceDocumentSnapshot current;
            SourceDocumentSnapshot original;
            SourceBatchDiskEvidence disk;
        };

        /** @brief Reverses a confirmed rename after its live document and disk evidence are rechecked. */
        bool CommitRenameRestore(const RenameRestore &restore, SourceFileOpenService *open, DurableFileSystem &files,
                                 const ProjectMutationCoordinator *mutations, const CancellationToken &lifetime);
        SourceEditBatch batch_;
        SourceDocumentSnapshot draft_;
        std::shared_ptr<const SourceDraftBatch> draftGroup_;
        SourceDocumentSnapshot renamed_;
        SourceDocumentSnapshot beforeRename_;

        /** @brief Owns the Trash paths and native identities observed by a completed deletion. */
        struct DeletedSource final {
            SourceDocumentSnapshot source;
            std::filesystem::path original;
            std::filesystem::path payload;
            std::filesystem::path manifest;
            std::optional<FileEntryIdentity> payloadIdentity;
            std::optional<FileEntryIdentity> manifestIdentity;
        };

        /** @brief Restores a witnessed payload, or returns the resulting conflict or recovery disposition. */
        static Proposals::SourceTransactionFeedback RestoreDeletedFile(DeletedSource &deleted, SourceFileOpenService &open,
                                                                       DurableFileSystem &files,
                                                                       const ProjectMutationCoordinator &mutations,
                                                                       const CancellationToken &lifetime);
        std::optional<DeletedSource> deleted_;
        std::shared_ptr<const Proposals::SourceReviewAudit> audit_;
        Proposals::SourceTransactionFeedback feedback_{};
        bool reverted_{};
        std::optional<Proposals::SourceReviewDisposition> committedFileOutcome_;

        using Candidate = std::variant<Proposals::PreparedSourceReview, RenameRestore>;

        struct Preparation final {
            JobHandle job;
            CancellationToken lifetime;
            std::shared_ptr<std::optional<Candidate>> output;
            Proposals::SourceTransactionFeedback failure{Proposals::SourceTransactionFeedback::ApplyFailed};
        };

        std::optional<Preparation> preparing_;

        struct HistoryRead final {
            JobHandle job;
            std::shared_ptr<std::shared_ptr<const Proposals::SourceReviewAudit>> latest;
        };

        struct HistoryWrite final {
            JobHandle job;
            std::shared_ptr<std::uint64_t> sequence;
        };

        std::optional<HistoryRead> auditRead_;
        std::optional<HistoryWrite> auditWrite_;
        std::uint64_t auditSequence_{};
        Proposals::SourceReviewDisposition scheduledDisposition_{};
        bool auditReadRequested_{};
        bool auditHistoryError_{};
    };
}  // namespace Horo::Editor
