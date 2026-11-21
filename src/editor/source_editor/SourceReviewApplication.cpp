#include "SourceReviewApplication.h"

#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "SourceReviewHistory.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <chrono>

namespace Horo::Editor {
    namespace {
        using Feedback = Proposals::SourceTransactionFeedback;

        /** @brief Separates irreversible native visibility from the directory durability result. */
        struct FileMove final {
            bool visible{};
            bool durable{};
        };

        /** @brief Calls the protected no-replace move without dropping the receipt on provider exceptions. */
        FileMove MoveObserved(DurableFileSystem &files, const std::filesystem::path &source, const std::filesystem::path &target) {
            AtomicFileReplacementReceipt receipt;
            try {
                const auto moved = files.MoveFileNoReplaceTracked(source, target, receipt);
                return {receipt.WasCommitted(), receipt.WasCommitted() && moved.HasValue()};
            } catch (...) {
                return {receipt.WasCommitted(), false};
            }
        }

        /** @brief Removes only a failed attempt's empty directory and owned metadata. */
        void RetireEmptyTrash(DurableFileSystem &files, const std::filesystem::path &manifest, const std::filesystem::path &directory,
                              const bool ownsManifest) noexcept {
            try {
                if (ownsManifest)
                    static_cast<void>(files.RemoveDurable(manifest));
                static_cast<void>(files.RemoveDurable(directory));
            } catch (...) {
                // Unpublished metadata may remain for inspection. No source payload is removed here.
            }
        }

        /** @brief Creates the exact local Trash directory after verifying physical project containment. */
        Result<std::filesystem::path> PrepareTrash(const std::filesystem::path &root, DurableFileSystem &files) {
            using Output = Result<std::filesystem::path>;
            const auto trash = root / ".horo/local/trash";
            std::error_code error;
            if (std::filesystem::weakly_canonical(trash, error) != trash || error)
                return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
            const auto parents = files.CreateDirectoryPath(trash, DirectoryCreationMode::Parents);
            if (parents.HasError())
                return Output::Failure(parents.ErrorValue());
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            auto attempt = trash / ("source-review-" + std::to_string(tick));
            const auto created = files.CreateDirectoryPath(attempt, DirectoryCreationMode::Single);
            if (created.HasError() || !created.Value())
                return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
            if (std::filesystem::weakly_canonical(attempt, error) != attempt || error)
                return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
            return Output::Success(std::move(attempt));
        }

        /** @brief Records the original relative path before the source can leave its original name. */
        bool WriteTrashManifest(DurableFileSystem &files, const std::filesystem::path &path, const SourceDocumentId &source,
                                bool &created) {
            const auto description = std::string{"horo.source-review-trash.v1\n"} + source.Value() + "\n";
            return files.WritePrivateDurable(path, std::as_bytes(std::span{description}), created).HasValue() &&
                   files.SyncDirectory(path.parent_path()).HasValue() && files.SyncDirectory(path.parent_path().parent_path()).HasValue();
        }

        /** @brief Captures a Trash file's no-follow witness only at its exact physical path. */
        std::optional<FileEntryIdentity> TrashFileIdentity(const DurableFileSystem &files, const std::filesystem::path &path) noexcept {
            try {
                std::error_code error;
                if (std::filesystem::weakly_canonical(path, error) != path || error)
                    return std::nullopt;
                const auto observed = files.InspectEntry(path);
                if (observed.HasError() || !observed.Value())
                    return std::nullopt;
                const auto &entry = *observed.Value();
                return entry.kind == FileEntryKind::RegularFile && entry.hardLinks == 1 ? entry.identity : std::nullopt;
            } catch (...) {
                return std::nullopt;
            }
        }

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

        /** @brief Moves a reviewed file while holding project mutation authority. */
        Result<SourceDocumentSnapshot> MoveReviewSource(SourceFileOpenService &open, const ProjectMutationCoordinator &mutations,
                                                        const SourceDocumentSnapshot &from, const SourceDocumentId &to, std::string text,
                                                        const SourceBatchDiskEvidence &disk, DurableFileSystem &files,
                                                        const CancellationToken &lifetime) {
            const auto lease = mutations.TryAcquire({open.ProjectRoot(), ProjectMutationOwner::Asset, "source-review-rename"});
            if (lease.HasError())
                return Result<SourceDocumentSnapshot>::Failure(lease.ErrorValue());
            return open.RenameReviewed(from, to, std::move(text), disk, files, lifetime);
        }

        /** @brief Surfaces uncertainty after either direction of a reviewed rename. */
        Feedback RenameFailure(const Error &error, const Feedback fallback) {
            return error.code.Value() == SourceDocumentErrors::SaveOutcomeUnknown.code.Value() ? Feedback::RenameRecovery
                                                                                               : FailureFeedback(error, fallback);
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
        const bool groupUndo = batch_.CanUndo() && std::ranges::all_of(batch_.Items(), [](const auto &item) {
            return !item.changed || item.snapshot.IsCurrent();
        });
        const bool renameUndo = renamed_.IsCurrent() && feedback_ != Feedback::RenameRecovery;
        return {feedback_,
                !preparing_ && (groupUndo || draft_.IsCurrent() || renameUndo || deleted_.has_value()),
                audit_,
                auditWrite_.has_value(),
                auditHistoryError_,
                Disposition()};
    }

    /** @copydoc SourceReviewTransactions::Disposition */
    Proposals::SourceReviewDisposition SourceReviewTransactions::Disposition() const noexcept {
        using enum Proposals::SourceReviewDisposition;
        if (reverted_)
            return Reverted;
        if (committedFileOutcome_)
            return *committedFileOutcome_;
        const auto &structuralBuffer = draft_.Revision() ? draft_ : renamed_;
        if (structuralBuffer.Revision()) {
            if (structuralBuffer.WasSavedInPlace())
                return Saved;
            return structuralBuffer.IsCurrent() ? Applied : Stale;
        }
        if (batch_.Items().empty()) {
            const auto previous = audit_ ? audit_->disposition : Historical;
            return previous == Applied ? Stale : previous;
        }
        bool sawSave{};
        bool needsSave{};
        for (const auto &item : batch_.Items()) {
            if (!item.changed)
                continue;
            if (item.snapshot.WasSavedInPlace()) {
                sawSave = true;
                continue;
            }
            if (!item.snapshot.IsCurrent())
                return Stale;
            needsSave = true;
        }
        return sawSave && !needsSave ? Saved : Applied;
    }

    /** @copydoc SourceReviewTransactions::QueueUndo */
    bool SourceReviewTransactions::QueueUndo(const CancellationToken &lifetime, EditorWorkspaceViewCommandData &command) const {
        if (command.command != EditorWorkspaceViewCommand::None || !Status().canUndo || lifetime.IsCancellationRequested())
            return false;
        command.sourceReviewUndo = batch_;
        command.sourceReviewDraftUndo = draft_;
        command.sourceReviewRenameUndo = renamed_;
        if (deleted_)
            command.sourceReviewDeleteUndo = deleted_->source;
        command.sourceReviewLifetime = lifetime;
        command.command = EditorWorkspaceViewCommand::UndoSourceReview;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Process */
    bool SourceReviewTransactions::Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents,
                                           const JobSystem *jobs, const std::filesystem::path &root, DurableFileSystem *files,
                                           SourceFileOpenService *open, const ProjectMutationCoordinator *mutations) {
        switch (command.command) {
            case EditorWorkspaceViewCommand::ApplySourceReview:
                Start(command.sourceReviewApplication, jobs, root, files, command.sourceReviewLifetime);
                return true;
            case EditorWorkspaceViewCommand::UndoSourceReview:
                if (preparing_)
                    return true;
                if (command.sourceReviewDraftUndo.Revision())
                    UndoDraft(command.sourceReviewDraftUndo, open, command.sourceReviewLifetime);
                else if (command.sourceReviewRenameUndo.Revision())
                    StartRenameRestore(command.sourceReviewRenameUndo, jobs, root, files, command.sourceReviewLifetime);
                else if (command.sourceReviewDeleteUndo.Revision())
                    UndoDelete(command.sourceReviewDeleteUndo, open, files, mutations, command.sourceReviewLifetime);
                else
                    Undo(command.sourceReviewUndo, documents, command.sourceReviewLifetime);
                return true;
            default:
                return false;
        }
    }

    /** @copydoc SourceReviewTransactions::StartRenameRestore */
    void SourceReviewTransactions::StartRenameRestore(const SourceDocumentSnapshot &receipt, const JobSystem *jobs,
                                                      const std::filesystem::path &root, const DurableFileSystem *files,
                                                      const CancellationToken &lifetime) {
        if (lifetime.IsCancellationRequested()) {
            feedback_ = Feedback::Cancelled;
            return;
        }
        if (feedback_ == Feedback::RenameRecovery)
            return;
        feedback_ = Feedback::Conflict;
        if (!renamed_.SharesStateWith(receipt) || !renamed_.IsCurrent())
            return;
        feedback_ = Feedback::UndoFailed;
        if (!jobs || !files)
            return;
        try {
            auto output = std::make_shared<std::optional<Candidate>>();
            auto job =
                jobs->SubmitResult({.parentCancellation = lifetime, .priority = JobPriority::Interactive, .resource = JobResource::Io},
                                   [restore = RenameRestore{renamed_, beforeRename_, {}}, output, root,
                                    files](const CancellationToken &stop) mutable {
                auto disk = SourceBatchDiskEvidence::Capture(root, std::span{&restore.current, 1}, *files, stop);
                if (disk.HasError())
                    return Result<void>::Failure(std::move(disk).ErrorValue());
                restore.disk = std::move(disk).Value();
                output->emplace(std::move(restore));
                return Result<void>::Success();
            });
            if (job.HasValue()) {
                preparing_.emplace(Preparation{std::move(job).Value(), lifetime, std::move(output), Feedback::UndoFailed});
                feedback_ = Feedback::Preparing;
            }
        } catch (const std::bad_alloc &) {
            // A rejected preparation retains the last committed rename receipt.
        }
    }

    /** @copydoc SourceReviewTransactions::Start */
    void SourceReviewTransactions::Start(std::shared_ptr<const SourceReviewApplication> request, const JobSystem *jobs,
                                         const std::filesystem::path &root, const DurableFileSystem *files,
                                         const CancellationToken &lifetime) {
        if (preparing_)
            return;
        feedback_ = lifetime.IsCancellationRequested() ? Feedback::Cancelled : Feedback::ApplyFailed;
        if (feedback_ == Feedback::Cancelled || auditWrite_ || !jobs || !files || !request || !request->plan)
            return;
        try {
            auto output = std::make_shared<std::optional<Candidate>>();
            auto job =
                jobs->SubmitResult({.parentCancellation = lifetime, .priority = JobPriority::Interactive, .resource = JobResource::Io},
                                   [request = std::move(request), output, root, files](const CancellationToken &stop) {
                auto operation = request->decisions.PrepareFileOperation(request->policy, *request->plan, stop);
                if (operation.HasError())
                    return Result<void>::Failure(std::move(operation).ErrorValue());
                if (auto candidate = std::move(operation).Value()) {
                    const auto checked = std::visit([&](auto &file) -> Result<void> {
                        if constexpr (requires { file.disk; }) {
                            auto disk = SourceBatchDiskEvidence::Capture(root, std::span{&file.source, 1}, *files, stop);
                            if (disk.HasError())
                                return Result<void>::Failure(std::move(disk).ErrorValue());
                            file.disk = std::move(disk).Value();
                        }
                        return Result<void>::Success();
                    }, *candidate);
                    if (checked.HasError())
                        return checked;
                    output->emplace(std::move(*candidate));
                    return Result<void>::Success();
                }
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
    bool SourceReviewTransactions::Poll(SourceDocumentService &documents, DurableFileSystem *files, SourceFileOpenService *open,
                                        WorkspacePanelHost *host, const ProjectMutationCoordinator *mutations) {
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
            feedback_ = error ? FailureFeedback(*error, preparation.failure) : preparation.failure;
            return false;
        }
        if (const auto *restore = std::get_if<RenameRestore>(&**preparation.output))
            return CommitRenameRestore(*restore, open, *files, mutations, preparation.lifetime);
        if (auto *operation = std::get_if<Proposals::PreparedSourceFileOperation>(&**preparation.output)) {
            if (auto *creation = std::get_if<Proposals::PreparedSourceCreate>(operation))
                return CommitDraft(*creation, open, host, preparation.lifetime);
            if (auto *rename = std::get_if<Proposals::PreparedSourceRename>(operation))
                return CommitRename(*rename, open, *files, mutations, preparation.lifetime);
            return CommitDelete(std::get<Proposals::PreparedSourceDelete>(*operation), open, *files, mutations, preparation.lifetime);
        }
        auto applied =
            std::get<Proposals::PreparedSourceReview>(**preparation.output).CommitObserved(documents, *files, preparation.lifetime);
        if (applied.HasError()) {
            feedback_ = FailureFeedback(applied.ErrorValue(), Feedback::ApplyFailed);
            return false;
        }
        auto outcome = std::move(applied).Value();
        batch_ = std::move(outcome.batch);
        draft_ = {};
        renamed_ = {};
        beforeRename_ = {};
        deleted_.reset();
        committedFileOutcome_.reset();
        audit_ = std::move(outcome.audit);
        reverted_ = false;
        auditSequence_ = 0;
        feedback_ = Feedback::Applied;
        return true;
    }

    /** @copydoc SourceReviewTransactions::CommitDraft */
    bool SourceReviewTransactions::CommitDraft(Proposals::PreparedSourceCreate &creation, SourceFileOpenService *open,
                                               WorkspacePanelHost *host, const CancellationToken &lifetime) {
        feedback_ = Feedback::ApplyFailed;
        if (!open)
            return false;
        const DocumentOpenKey key{DocumentKind::Source, creation.file.path};
        auto staged = open->StageNew(creation.file.path, std::move(creation.file.text), lifetime);
        if (staged.HasError()) {
            feedback_ = FailureFeedback(staged.ErrorValue(), Feedback::ApplyFailed);
            return false;
        }
        if (host) {
            const auto instance = staged.Value().Identity().instance;
            bool presented{};
            try {
                presented = host->OpenDocument(key).HasValue();
            } catch (const std::bad_alloc &) {
                // A partially inserted tab is reconciled from the registry below.
            }
            if (!presented) {
                static_cast<void>(open->CloseDocument(instance, true));
                host->RefreshDocuments();
                return false;
            }
        }
        draft_ = std::move(staged).Value();
        renamed_ = {};
        beforeRename_ = {};
        deleted_.reset();
        batch_ = {};
        committedFileOutcome_.reset();
        audit_ = std::move(creation.audit);
        reverted_ = false;
        auditSequence_ = 0;
        feedback_ = Feedback::Applied;
        return true;
    }

    /** @copydoc SourceReviewTransactions::CommitDelete */
    bool SourceReviewTransactions::CommitDelete(Proposals::PreparedSourceDelete &deletion, SourceFileOpenService *open,
                                                DurableFileSystem &files, const ProjectMutationCoordinator *mutations,
                                                const CancellationToken &lifetime) {
        feedback_ = Feedback::ApplyFailed;
        if (!open || !mutations)
            return false;
        const auto &root = open->ProjectRoot();
        const auto identity = deletion.source.Identity();
        const auto current = open->Documents().Snapshot(identity.instance);
        if (current.HasError() || !deletion.source.SharesStateWith(current.Value()) || deletion.source.Dirty()) {
            feedback_ = Feedback::Conflict;
            return false;
        }
        const auto project = mutations->TryAcquire({root, ProjectMutationOwner::Asset, "source-review-trash"});
        if (project.HasError())
            return false;
        const auto path = deletion.disk.RecheckSingle(root, deletion.source, files, lifetime);
        if (path.HasError()) {
            feedback_ = FailureFeedback(path.ErrorValue(), Feedback::ApplyFailed);
            return false;
        }
        auto lockPath = path.Value();
        lockPath += ".horo-source.lock";
        const auto lease = files.TryAcquireExclusive(lockPath, "horo.source_document");
        if (lease.HasError())
            return false;
        const auto trash = PrepareTrash(root, files);
        if (trash.HasError())
            return false;
        const auto manifest = trash.Value() / "source-review-manifest";
        const auto saved = trash.Value() / "payload";
        bool ownsManifest{};
        if (!WriteTrashManifest(files, manifest, identity.key.source, ownsManifest)) {
            RetireEmptyTrash(files, manifest, trash.Value(), ownsManifest);
            return false;
        }
        DeletedSource receipt{deletion.source, path.Value(), saved, manifest, {}, TrashFileIdentity(files, manifest)};
        if (const auto latest = deletion.disk.RecheckSingle(root, deletion.source, files, lifetime); latest.HasError()) {
            feedback_ = FailureFeedback(latest.ErrorValue(), Feedback::ApplyFailed);
            RetireEmptyTrash(files, manifest, trash.Value(), ownsManifest);
            return false;
        }
        if (lifetime.IsCancellationRequested()) {
            RetireEmptyTrash(files, manifest, trash.Value(), ownsManifest);
            feedback_ = Feedback::Cancelled;
            return false;
        }
        const auto moved = MoveObserved(files, path.Value(), saved);
        if (!moved.visible) {
            RetireEmptyTrash(files, manifest, trash.Value(), ownsManifest);
            return false;
        }
        bool closed{};
        if (moved.durable)
            receipt.payloadIdentity = TrashFileIdentity(files, saved);
        if (receipt.payloadIdentity && deletion.source.IsCurrent()) {
            try {
                closed = open->CloseDocument(identity.instance).HasValue();
            } catch (const std::bad_alloc &) {
                // Restore the original name if document retirement could not finish.
            }
        }
        if (!closed) {
            const auto restored = MoveObserved(files, saved, path.Value());
            if (restored.visible) {
                if (restored.durable)
                    RetireEmptyTrash(files, manifest, trash.Value(), ownsManifest);
                else
                    feedback_ = Feedback::DeleteRecovery;
                return false;
            }
        }
        batch_ = {};
        draft_ = {};
        renamed_ = {};
        beforeRename_ = {};
        deleted_.reset();
        if (closed)
            deleted_.emplace(std::move(receipt));
        committedFileOutcome_ = closed ? Proposals::SourceReviewDisposition::Saved : Proposals::SourceReviewDisposition::Stale;
        audit_ = std::move(deletion.audit);
        reverted_ = false;
        auditSequence_ = 0;
        feedback_ = closed ? Feedback::Deleted : Feedback::DeleteRecovery;
        return true;
    }

    /** @copydoc SourceReviewTransactions::CommitRename */
    bool SourceReviewTransactions::CommitRename(Proposals::PreparedSourceRename &rename, SourceFileOpenService *open,
                                                DurableFileSystem &files, const ProjectMutationCoordinator *mutations,
                                                const CancellationToken &lifetime) {
        feedback_ = Feedback::ApplyFailed;
        if (!open || !mutations)
            return false;
        auto moved =
            MoveReviewSource(*open, *mutations, rename.source, rename.destination, std::move(rename.text), rename.disk, files, lifetime);
        if (moved.HasError()) {
            feedback_ = RenameFailure(moved.ErrorValue(), Feedback::ApplyFailed);
            return false;
        }
        batch_ = {};
        draft_ = {};
        renamed_ = std::move(moved).Value();
        beforeRename_ = std::move(rename.source);
        deleted_.reset();
        committedFileOutcome_.reset();
        if (!renamed_.Dirty())
            committedFileOutcome_ = Proposals::SourceReviewDisposition::Saved;
        audit_ = std::move(rename.audit);
        reverted_ = false;
        auditSequence_ = 0;
        feedback_ = Feedback::Renamed;
        return true;
    }

    /** @copydoc SourceReviewTransactions::CommitRenameRestore */
    bool SourceReviewTransactions::CommitRenameRestore(const RenameRestore &restore, SourceFileOpenService *open, DurableFileSystem &files,
                                                       const ProjectMutationCoordinator *mutations, const CancellationToken &lifetime) {
        feedback_ = Feedback::UndoFailed;
        if (!open || !mutations)
            return false;
        try {
            const auto restored = MoveReviewSource(*open, *mutations, restore.current, restore.original.Identity().key.source,
                                                   std::string{restore.original.Text()}, restore.disk, files, lifetime);
            if (restored.HasError()) {
                feedback_ = RenameFailure(restored.ErrorValue(), Feedback::UndoFailed);
                if (feedback_ == Feedback::RenameRecovery) {
                    committedFileOutcome_ = Proposals::SourceReviewDisposition::Stale;
                    renamed_ = {};
                    beforeRename_ = {};
                }
                return false;
            }
            renamed_ = {};
            beforeRename_ = {};
            reverted_ = true;
            feedback_ = Feedback::Undone;
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        }
    }

    /** @copydoc SourceReviewTransactions::UpdateAuditHistory */
    void SourceReviewTransactions::UpdateAuditHistory(const JobSystem *jobs, const std::filesystem::path &root,
                                                      const ProjectMutationCoordinator *mutations, DurableFileSystem *files) {
        if (auditRead_) {
            if (const auto snapshot = auditRead_->job.Snapshot(); snapshot && snapshot->terminalResult) {
                if (!audit_) {
                    auditHistoryError_ = snapshot->state != JobState::Succeeded;
                    audit_ = std::move(*auditRead_->latest);
                }
                auditRead_.reset();
            }
        }
        PollHistoryWrite();
        if (auditSequence_ && !auditWrite_ && Disposition() != scheduledDisposition_)
            RecordAppliedAudit(jobs, root, mutations, files);
        BeginHistoryRead(jobs, root);
    }

    /** @copydoc SourceReviewTransactions::PollHistoryWrite */
    void SourceReviewTransactions::PollHistoryWrite() {
        if (!auditWrite_)
            return;
        const auto snapshot = auditWrite_->job.Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            return;
        auditHistoryError_ = snapshot->state != JobState::Succeeded;
        if (!auditHistoryError_)
            auditSequence_ = *auditWrite_->sequence;
        auditWrite_.reset();
    }

    /** @copydoc SourceReviewTransactions::BeginHistoryRead */
    void SourceReviewTransactions::BeginHistoryRead(const JobSystem *jobs, const std::filesystem::path &root) {
        if (auditReadRequested_ || !jobs || root.empty())
            return;
        auditReadRequested_ = true;
        try {
            auto latest = std::make_shared<std::shared_ptr<const Proposals::SourceReviewAudit>>();
            auto job = jobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                          [root, latest](const CancellationToken &stop) {
                auto history = ReadSourceReviewHistory(root, stop);
                if (history.HasError())
                    return Result<void>::Failure(history.ErrorValue());
                *latest = std::move(history).Value();
                return Result<void>::Success();
            });
            if (job.HasValue()) {
                auditRead_.emplace(HistoryRead{std::move(job).Value(), std::move(latest)});
                return;
            }
        } catch (const std::bad_alloc &) {
            // Reading attribution is independent from admitting live source documents.
        }
        auditHistoryError_ = true;
    }

    /** @copydoc SourceReviewTransactions::DrainAuditHistory */
    bool SourceReviewTransactions::DrainAuditHistory(const JobSystem *jobs, const std::filesystem::path &root,
                                                     const ProjectMutationCoordinator *mutations, DurableFileSystem *files) {
        constexpr JoinOptions join{WaitPolicy::OwnerThreadBlockAllowed, Duration::FromMilliseconds(1000)};
        if (auditWrite_) {
            static_cast<void>(auditWrite_->job.Wait(join));
            PollHistoryWrite();
        }
        if (auditWrite_)
            return false;
        if (auditSequence_ && Disposition() != scheduledDisposition_)
            RecordAppliedAudit(jobs, root, mutations, files);
        if (auditWrite_) {
            static_cast<void>(auditWrite_->job.Wait(join));
            PollHistoryWrite();
        }
        return !auditWrite_ && !auditHistoryError_;
    }

    /** @copydoc SourceReviewTransactions::RecordAppliedAudit */
    void SourceReviewTransactions::RecordAppliedAudit(const JobSystem *jobs, const std::filesystem::path &root,
                                                      const ProjectMutationCoordinator *mutations, DurableFileSystem *files) {
        if (!audit_ || auditWrite_ || !jobs || !mutations || !files)
            return;
        scheduledDisposition_ = Disposition();
        try {
            auto sequence = std::make_shared<std::uint64_t>(auditSequence_);
            auto job = jobs->SubmitResult({.priority = JobPriority::Interactive, .resource = JobResource::Io},
                                          [root, record = audit_, mutations, files, sequence,
                                           disposition = scheduledDisposition_](const CancellationToken &stop) {
                const auto stored = WriteSourceReviewHistory(root, *record, *sequence, disposition, *mutations, *files, stop);
                if (stored.HasError())
                    return Result<void>::Failure(stored.ErrorValue());
                *sequence = stored.Value();
                return Result<void>::Success();
            });
            if (job.HasValue()) {
                auditWrite_.emplace(HistoryWrite{std::move(job).Value(), std::move(sequence)});
                return;
            }
        } catch (const std::bad_alloc &) {
            // A storage submission failure leaves the exact live receipt and its undo available.
        }
        auditHistoryError_ = true;
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
        reverted_ = true;
        feedback_ = Feedback::Undone;
    }

    /** @copydoc SourceReviewTransactions::UndoDraft */
    void SourceReviewTransactions::UndoDraft(const SourceDocumentSnapshot &receipt, SourceFileOpenService *open,
                                             const CancellationToken &lifetime) {
        if (lifetime.IsCancellationRequested()) {
            feedback_ = Feedback::Cancelled;
            return;
        }
        feedback_ = Feedback::Conflict;
        if (!open || !draft_.SharesStateWith(receipt) || !draft_.IsCurrent())
            return;
        const auto undone = open->CloseDocument(receipt.Identity().instance, true);
        if (undone.HasError()) {
            feedback_ = FailureFeedback(undone.ErrorValue(), Feedback::UndoFailed);
            return;
        }
        draft_ = {};
        reverted_ = true;
        feedback_ = Feedback::Undone;
    }

    /** @copydoc SourceReviewTransactions::RestoreDeletedFile */
    Feedback SourceReviewTransactions::RestoreDeletedFile(DeletedSource &deleted, SourceFileOpenService &open, DurableFileSystem &files,
                                                          const ProjectMutationCoordinator &mutations, const CancellationToken &lifetime) {
        const auto &root = open.ProjectRoot();
        const auto key = deleted.source.Identity().key;
        const auto &relative = key.source.Value();
        if (root / std::u8string{relative.begin(), relative.end()} != deleted.original)
            return Feedback::Conflict;
        const auto project = mutations.TryAcquire({root, ProjectMutationOwner::Asset, "source-review-trash"});
        if (project.HasError())
            return Feedback::UndoFailed;
        const auto available = [&] {
            return Proposals::ObserveSourceDestination(key.source, root, files, lifetime) == Proposals::SourcePathState::Absent;
        };
        if (open.DocumentRegistry().Find(key) || !available())
            return lifetime.IsCancellationRequested() ? Feedback::Cancelled : Feedback::Conflict;
        auto lockPath = deleted.original;
        lockPath += ".horo-source.lock";
        const auto lease = files.TryAcquireExclusive(lockPath, "horo.source_document");
        if (lease.HasError())
            return Feedback::UndoFailed;
        const auto trash = deleted.payload.parent_path();
        if (!available() || TrashFileIdentity(files, deleted.payload) != deleted.payloadIdentity)
            return lifetime.IsCancellationRequested() ? Feedback::Cancelled : Feedback::Conflict;
        if (lifetime.IsCancellationRequested())
            return Feedback::Cancelled;

        const auto restored = MoveObserved(files, deleted.payload, deleted.original);
        if (restored.durable) {
            const bool ownsManifest = deleted.manifestIdentity && TrashFileIdentity(files, deleted.manifest) == deleted.manifestIdentity;
            RetireEmptyTrash(files, deleted.manifest, trash, ownsManifest);
            return Feedback::Undone;
        }
        if (!restored.visible)
            return Feedback::Conflict;
        const auto returned = MoveObserved(files, deleted.original, deleted.payload);
        if (returned.durable) {
            deleted.payloadIdentity = TrashFileIdentity(files, deleted.payload);
            if (deleted.payloadIdentity)
                return Feedback::UndoFailed;
        }
        return Feedback::DeleteRecovery;
    }

    /** @copydoc SourceReviewTransactions::UndoDelete */
    void SourceReviewTransactions::UndoDelete(const SourceDocumentSnapshot &receipt, SourceFileOpenService *open, DurableFileSystem *files,
                                              const ProjectMutationCoordinator *mutations, const CancellationToken &lifetime) {
        if (lifetime.IsCancellationRequested()) {
            feedback_ = Feedback::Cancelled;
            return;
        }
        feedback_ = Feedback::Conflict;
        if (!deleted_ || !deleted_->source.SharesStateWith(receipt))
            return;
        feedback_ = Feedback::UndoFailed;
        if (!open || !files || !mutations)
            return;
        try {
            feedback_ = RestoreDeletedFile(*deleted_, *open, *files, *mutations, lifetime);
            if (feedback_ == Feedback::Undone) {
                deleted_.reset();
                reverted_ = true;
            } else if (feedback_ == Feedback::DeleteRecovery) {
                deleted_.reset();
                committedFileOutcome_ = Proposals::SourceReviewDisposition::Stale;
            }
        } catch (const std::bad_alloc &) {
            // An allocation failure during admission leaves the Trash payload untouched.
        }
    }
}  // namespace Horo::Editor
