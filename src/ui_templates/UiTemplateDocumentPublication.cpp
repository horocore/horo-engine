#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateOwnerState.h"

#include <exception>

namespace Horo::UiTemplates {
    namespace UiErrors {
        namespace {
            const ErrorDomainId UiDomain{"horo.runtime_ui"};
        }

        /** @copydoc TemplateDocumentShutdown */
        const ErrorCodeDescriptor TemplateDocumentShutdown{UiDomain,
                                                           ErrorCode{"runtime_ui.template.document_shutdown"},
                                                           ErrorSeverity::Error,
                                                           "Template document authoring admission is closed.",
                                                           "Prepare through a live document owner.",
                                                           false,
                                                           true};
        /** @copydoc TemplateRebaseInvalid */
        const ErrorCodeDescriptor TemplateRebaseInvalid{UiDomain,
                                                        ErrorCode{"runtime_ui.template.rebase_invalid"},
                                                        ErrorSeverity::Error,
                                                        "Template rebase requires an explicit data-preserving migration.",
                                                        "Review the rejected parameter or populated slot.",
                                                        false,
                                                        true};
        /** @copydoc TemplateDocumentOwnerMismatch */
        const ErrorCodeDescriptor TemplateDocumentOwnerMismatch{UiDomain,
                                                                ErrorCode{"runtime_ui.template.document_owner_mismatch"},
                                                                ErrorSeverity::Error,
                                                                "The prepared transaction belongs to another document owner.",
                                                                "Prepare again against the intended owner.",
                                                                false,
                                                                true};
        /** @copydoc TemplateDocumentWrongThread */
        const ErrorCodeDescriptor TemplateDocumentWrongThread{UiDomain,
                                                              ErrorCode{"runtime_ui.template.document_wrong_thread"},
                                                              ErrorSeverity::Error,
                                                              "Template authoring operation requires its creation thread.",
                                                              "Queue the command for the sole document/runtime owner safe point.",
                                                              false,
                                                              true};
        /** @copydoc TemplateDocumentCancelled */
        const ErrorCodeDescriptor TemplateDocumentCancelled{UiDomain,
                                                            ErrorCode{"runtime_ui.template.document_cancelled"},
                                                            ErrorSeverity::Error,
                                                            "Template document preparation or publication was cancelled.",
                                                            "Prepare a new explicitly admitted transaction.",
                                                            false,
                                                            true};
        /** @copydoc TemplateUndoEmpty */
        const ErrorCodeDescriptor TemplateUndoEmpty{UiDomain,
                                                    ErrorCode{"runtime_ui.template.undo_empty"},
                                                    ErrorSeverity::Error,
                                                    "No prior template document is retained for undo.",
                                                    "Continue from the current accepted document.",
                                                    false,
                                                    true};
        /** @copydoc TemplatePersistenceMalformed */
        const ErrorCodeDescriptor TemplatePersistenceMalformed{UiDomain,
                                                               ErrorCode{"runtime_ui.template.persistence_malformed"},
                                                               ErrorSeverity::Error,
                                                               "Linked UI source is malformed or unsupported.",
                                                               "Review source schema and typed identities without dropping authored data.",
                                                               false,
                                                               true};
    }  // namespace UiErrors

    /** @copydoc UiTemplateDocumentOwner::PreparePublication */
    UiTemplatePreparedTransaction UiTemplateDocumentOwner::PreparePublication(std::shared_ptr<const Detail::UiTemplateOwnerState> next,
                                                                              const CancellationToken &cancellation) const noexcept {
        return UiTemplatePreparedTransaction{state_, std::move(next), identity_, cancellation,
                                             undo_.size() == undoLimit_ ? undo_.front() : nullptr};
    }

    /** @copydoc UiTemplateDocumentOwner::Commit */
    Result<UiTemplateDocumentSnapshot> UiTemplateDocumentOwner::Commit(const UiTemplatePreparedTransaction &candidate) {
        if (const auto valid = CanCommit(candidate); valid.HasError())
            return Result<UiTemplateDocumentSnapshot>::Failure(valid.ErrorValue());
        CommitValidated(candidate);
        return Result<UiTemplateDocumentSnapshot>::Success(Capture());
    }

    /** @copydoc UiTemplateDocumentOwner::CanCommit */
    Result<void> UiTemplateDocumentOwner::CanCommit(const UiTemplatePreparedTransaction &candidate) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return thread;
        if (!identity_)
            return Result<void>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
        if (candidate.owner_.lock() != identity_)
            return Result<void>::Failure(MakeError(UiErrors::TemplateDocumentOwnerMismatch));
        if (candidate.base_ != state_)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
        if (candidate.cancellation_.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
        return Result<void>::Success();
    }

    /** @copydoc UiTemplateDocumentOwner::CheckOwnerThread */
    Result<void> UiTemplateDocumentOwner::CheckOwnerThread() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::TemplateDocumentWrongThread));
        return Result<void>::Success();
    }

    /** @copydoc UiTemplateDocumentOwner::CheckPreparation */
    Result<void> UiTemplateDocumentOwner::CheckPreparation(const CancellationToken &cancellation) const {
        if (!identity_)
            return Result<void>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
        return Result<void>::Success();
    }

    /** @copydoc UiTemplateDocumentOwner::CommitValidated */
    void UiTemplateDocumentOwner::CommitValidated(const UiTemplatePreparedTransaction &candidate) noexcept {
        // Admission proves a positive bound and construction reserves the full history capacity.
        // The prepared transaction also pins evicted history, preventing final catalog destruction in this safe point.
        if (candidate.undo_) {
            undo_.pop_back();
        } else {
            if (undo_.size() == undoLimit_)
                undo_.erase(undo_.begin());
            undo_.push_back(state_);
        }
        state_ = candidate.next_;
    }

    /** @copydoc UiTemplateDocumentOwner::Undo */
    Result<UiTemplateDocumentSnapshot> UiTemplateDocumentOwner::Undo() {
        const auto prepared = PrepareUndo();
        if (prepared.HasError())
            return Result<UiTemplateDocumentSnapshot>::Failure(prepared.ErrorValue());
        return Commit(prepared.Value());
    }

    /** @copydoc UiTemplateDocumentOwner::Shutdown */
    void UiTemplateDocumentOwner::Shutdown() noexcept {
        if (ownerThread_ != std::this_thread::get_id())
            std::terminate();
        identity_.reset();
        undo_.clear();
    }
}  // namespace Horo::UiTemplates
