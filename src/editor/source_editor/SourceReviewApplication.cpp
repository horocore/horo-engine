#include "SourceReviewApplication.h"

#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "SourceReviewHistory.h"
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
                auto creation = request->decisions.PrepareCreate(request->policy, *request->plan, stop);
                if (creation.HasError())
                    return Result<void>::Failure(std::move(creation).ErrorValue());
                if (auto draft = std::move(creation).Value()) {
                    output->emplace(std::move(*draft));
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
    bool SourceReviewTransactions::Poll(SourceDocumentService &documents, const DurableFileSystem *files, SourceFileOpenService *open,
                                        WorkspacePanelHost *host) {
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
        if (auto *creation = std::get_if<Proposals::PreparedSourceCreate>(&**preparation.output))
            return CommitDraft(*creation, open, host, preparation.lifetime);
        auto applied =
            std::get<Proposals::PreparedSourceReview>(**preparation.output).CommitObserved(documents, *files, preparation.lifetime);
        if (applied.HasError()) {
            feedback_ = FailureFeedback(applied.ErrorValue(), Feedback::ApplyFailed);
            return false;
        }
        auto outcome = std::move(applied).Value();
        batch_ = std::move(outcome.batch);
        draft_ = {};
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
        audit_ = std::move(creation.audit);
        reverted_ = false;
        auditSequence_ = 0;
        feedback_ = Feedback::Applied;
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
