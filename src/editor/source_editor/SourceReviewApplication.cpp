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
        const bool groupUndo = batch_.CanUndo() && std::ranges::all_of(batch_.Items(), [](const auto &item) {
            return !item.changed || item.snapshot.IsCurrent();
        });
        return {feedback_,    !preparing_ && (groupUndo || draft_.IsCurrent()), audit_, auditWrite_.has_value(), auditHistoryError_,
                Disposition()};
    }

    /** @copydoc SourceReviewTransactions::Disposition */
    Proposals::SourceReviewDisposition SourceReviewTransactions::Disposition() const noexcept {
        using enum Proposals::SourceReviewDisposition;
        if (reverted_)
            return Reverted;
        if (removal_)
            return *removal_;
        if (draft_.Revision()) {
            if (draft_.WasSavedInPlace())
                return Saved;
            return draft_.IsCurrent() ? Applied : Stale;
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
        command.sourceReviewLifetime = lifetime;
        command.command = EditorWorkspaceViewCommand::UndoSourceReview;
        return true;
    }

    /** @copydoc SourceReviewTransactions::Process */
    bool SourceReviewTransactions::Process(const EditorWorkspaceViewCommandData &command, SourceDocumentService &documents,
                                           const JobSystem *jobs, const std::filesystem::path &root, const DurableFileSystem *files,
                                           SourceFileOpenService *open) {
        switch (command.command) {
            case EditorWorkspaceViewCommand::ApplySourceReview:
                Start(command.sourceReviewApplication, jobs, root, files, command.sourceReviewLifetime);
                return true;
            case EditorWorkspaceViewCommand::UndoSourceReview:
                if (preparing_)
                    return true;
                if (command.sourceReviewDraftUndo.Revision())
                    UndoDraft(command.sourceReviewDraftUndo, open, command.sourceReviewLifetime);
                else
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
                    if (auto *deletion = std::get_if<Proposals::PreparedSourceDelete>(&*candidate)) {
                        auto disk = SourceBatchDiskEvidence::Capture(root, std::span{&deletion->source, 1}, *files, stop);
                        if (disk.HasError())
                            return Result<void>::Failure(std::move(disk).ErrorValue());
                        deletion->disk = std::move(disk).Value();
                    }
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
            feedback_ = error ? FailureFeedback(*error, Feedback::ApplyFailed) : Feedback::ApplyFailed;
            return false;
        }
        if (auto *operation = std::get_if<Proposals::PreparedSourceFileOperation>(&**preparation.output)) {
            if (auto *creation = std::get_if<Proposals::PreparedSourceCreate>(operation))
                return CommitDraft(*creation, open, host, preparation.lifetime);
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
        removal_.reset();
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
        batch_ = {};
        removal_.reset();
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
        if (moved.durable && deletion.source.IsCurrent()) {
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
        removal_ = closed ? Proposals::SourceReviewDisposition::Saved : Proposals::SourceReviewDisposition::Stale;
        audit_ = std::move(deletion.audit);
        reverted_ = false;
        auditSequence_ = 0;
        feedback_ = closed ? Feedback::Deleted : Feedback::DeleteRecovery;
        return true;
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
}  // namespace Horo::Editor
