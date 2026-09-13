#include "Horo/Runtime/Save/SaveSlotCommitTransaction.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] Error Invalid(std::string message) {
            return MakeError(SaveErrors::SlotCommitInvalid, std::move(message));
        }

        [[nodiscard]] Error RecoveryFailure(Error cause, std::string message) {
            return WrapError(SaveErrors::SlotCommitRecoveryFailed, std::move(cause), std::move(message));
        }

        [[nodiscard]] bool ValidPhase(const SaveSlotCommitPhase phase) noexcept {
            return phase == SaveSlotCommitPhase::Preparing || phase == SaveSlotCommitPhase::Prepared ||
                   phase == SaveSlotCommitPhase::Publishing || phase == SaveSlotCommitPhase::Published;
        }

        [[nodiscard]] Result<void> ValidateJournalIdentity(const SaveSlotCommitJournal &journal) {
            if (journal.operation == 0 || journal.address.namespaceAccess.expectedRevision == 0 ||
                !journal.address.namespaceAccess.expected.IsValid() || !journal.address.slot.IsValid() || !ValidPhase(journal.phase))
                return Result<void>::Failure(Invalid("Slot commit journal identity, namespace binding, or phase is invalid."));
            if (journal.candidate.publication.slot != journal.address.slot)
                return Result<void>::Failure(Invalid("Slot commit candidate does not match the addressed logical slot."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCandidate(const SaveSlotCatalogEntry &candidate) {
            if (auto valid = ValidateSaveSlotPublicationMetadata(candidate.publication); valid.HasError())
                return Result<void>::Failure(Invalid("Slot commit candidate metadata is invalid."));
            if (auto valid = ValidateSaveSlotDisplayMetadata(candidate.display); valid.HasError())
                return Result<void>::Failure(Invalid("Slot commit candidate display metadata is invalid."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePrevious(const SaveSlotCommitJournal &journal) {
            if (!journal.previous)
                return Result<void>::Success();
            if (auto valid = ValidateSaveSlotDisplayMetadata(journal.previous->display); valid.HasError())
                return Result<void>::Failure(Invalid("Previous slot display metadata is invalid."));
            if (auto valid = ValidateSaveSlotPublicationReplacement(journal.previous->publication, journal.candidate.publication);
                valid.HasError())
                return Result<void>::Failure(Invalid("Slot commit candidate is not a valid generation replacement."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateJournal(const SaveSlotCommitJournal &journal) {
            if (auto valid = ValidateJournalIdentity(journal); valid.HasError())
                return valid;
            if (auto valid = ValidateCandidate(journal.candidate); valid.HasError())
                return valid;
            return ValidatePrevious(journal);
        }

        [[nodiscard]] bool IsPublishedCandidate(const SaveSlotCommitJournal &journal,
                                                const SaveSlotCommitObservation &observation) noexcept {
            return observation.published && *observation.published == journal.candidate;
        }

        [[nodiscard]] bool IsPreviousOrEmpty(const SaveSlotCommitJournal &journal, const SaveSlotCommitObservation &observation) noexcept {
            return observation.published == journal.previous;
        }

        [[nodiscard]] Result<SaveSlotRecoveryAction> RemovePublishedJournal(ISaveSlotCommitStore &store,
                                                                            const SaveSlotCommitJournal &journal,
                                                                            SaveSlotRecoveryAction action) {
            if (auto removed = store.RemoveJournal(journal); removed.HasError())
                return Result<SaveSlotRecoveryAction>::Failure(
                    RecoveryFailure(removed.ErrorValue(), "The published generation is durable but its recovery journal remains."));
            return Result<SaveSlotRecoveryAction>::Success(action);
        }

        [[nodiscard]] bool Addresses(const SaveSlotCommitJournal &journal, const SaveStorageAddress &address) noexcept {
            return journal.address.namespaceAccess.expected == address.namespaceAccess.expected &&
                   journal.address.namespaceAccess.expectedRevision == address.namespaceAccess.expectedRevision &&
                   journal.address.slot == address.slot;
        }

        [[nodiscard]] Result<void> ValidateAdmission(ISaveSlotCommitStore &store, const SaveSlotCommitJournal &journal) {
            const auto existingJournal = store.LoadJournal(journal.address);
            if (existingJournal.HasError())
                return Result<void>::Failure(existingJournal.ErrorValue());
            if (existingJournal.Value())
                return Result<void>::Failure(
                    Invalid("The slot already has an unfinished transaction; recover it before admitting another commit."));
            const auto initial = store.Observe(journal);
            if (initial.HasError())
                return Result<void>::Failure(initial.ErrorValue());
            if (!IsPreviousOrEmpty(journal, initial.Value()))
                return Result<void>::Failure(Invalid("The supplied previous generation is stale relative to the current catalog."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> PrepareForPublishing(ISaveSlotCommitStore &store, SaveSlotCommitJournal &journal,
                                                        const ImmutableSaveArchive &archive) {
            if (auto stored = store.StoreJournal(journal); stored.HasError())
                return stored;
            if (auto prepared = store.PrepareGeneration(journal, archive); prepared.HasError())
                return prepared;
            journal.phase = SaveSlotCommitPhase::Prepared;
            if (auto stored = store.StoreJournal(journal); stored.HasError())
                return stored;
            journal.phase = SaveSlotCommitPhase::Publishing;
            return store.StoreJournal(journal);
        }

        [[nodiscard]] Result<void> Publish(ISaveSlotCommitStore &store, SaveSlotCommitJournal &journal) {
            if (auto published = store.PublishGeneration(journal); published.HasError())
                return Result<void>::Failure(
                    WrapError(SaveErrors::SlotCommitOutcomeUnknown, published.ErrorValue(),
                              "Atomic catalog publication failed with an unknown old-or-new outcome; recover under the same slot lease."));
            journal.phase = SaveSlotCommitPhase::Published;
            if (auto stored = store.StoreJournal(journal); stored.HasError())
                return Result<void>::Failure(
                    WrapError(SaveErrors::SlotCommitOutcomeUnknown, stored.ErrorValue(),
                              "The candidate was published but its committed recovery evidence could not be advanced."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<SaveSlotRecoveryAction> RecoverUnpublished(ISaveSlotCommitStore &store, const SaveSlotCommitJournal &journal,
                                                                        const SaveSlotCommitObservation &observation) {
            if (observation.candidatePrepared) {
                if (auto discarded = store.DiscardPrepared(journal); discarded.HasError())
                    return Result<SaveSlotRecoveryAction>::Failure(
                        RecoveryFailure(discarded.ErrorValue(), "Unpublished candidate generation could not be discarded."));
            }
            if (auto removed = store.RemoveJournal(journal); removed.HasError())
                return Result<SaveSlotRecoveryAction>::Failure(
                    RecoveryFailure(removed.ErrorValue(), "Unpublished slot journal could not be removed."));
            return Result<SaveSlotRecoveryAction>::Success(SaveSlotRecoveryAction::DiscardedUnpublished);
        }

        [[nodiscard]] Result<SaveSlotRecoveryAction> RecoverPublishing(ISaveSlotCommitStore &store, SaveSlotCommitJournal journal,
                                                                       const SaveSlotCommitObservation &observation) {
            if (!observation.candidatePrepared)
                return Result<SaveSlotRecoveryAction>::Failure(
                    Invalid("Publishing journal has no complete prepared candidate generation."));
            if (auto published = store.PublishGeneration(journal); published.HasError())
                return Result<SaveSlotRecoveryAction>::Failure(
                    RecoveryFailure(published.ErrorValue(), "Atomic catalog publication remains outcome-unknown."));
            journal.phase = SaveSlotCommitPhase::Published;
            if (auto stored = store.StoreJournal(journal); stored.HasError())
                return Result<SaveSlotRecoveryAction>::Failure(
                    RecoveryFailure(stored.ErrorValue(), "Recovered publication could not persist its finalized phase."));
            return RemovePublishedJournal(store, journal, SaveSlotRecoveryAction::PublishedCandidate);
        }

        [[nodiscard]] Result<SaveSlotRecoveryAction> RecoverObserved(ISaveSlotCommitStore &store, SaveSlotCommitJournal journal,
                                                                     const SaveSlotCommitObservation &observation) {
            const bool candidatePublished = IsPublishedCandidate(journal, observation);
            if (journal.phase == SaveSlotCommitPhase::Published && !candidatePublished)
                return Result<SaveSlotRecoveryAction>::Failure(
                    Invalid("A published journal does not match the catalog's selected generation."));
            if (journal.phase == SaveSlotCommitPhase::Published)
                return RemovePublishedJournal(store, journal, SaveSlotRecoveryAction::FinalizedPublished);
            if (candidatePublished)
                return RemovePublishedJournal(store, journal, SaveSlotRecoveryAction::PublishedCandidate);
            if (!IsPreviousOrEmpty(journal, observation))
                return Result<SaveSlotRecoveryAction>::Failure(
                    Invalid("Slot recovery found an unrelated catalog generation and will not overwrite it."));
            if (journal.phase == SaveSlotCommitPhase::Preparing || journal.phase == SaveSlotCommitPhase::Prepared)
                return RecoverUnpublished(store, journal, observation);
            return RecoverPublishing(store, std::move(journal), observation);
        }
    }  // namespace

    /** @copydoc SaveSlotCommitTransaction::SaveSlotCommitTransaction */
    SaveSlotCommitTransaction::SaveSlotCommitTransaction(ISaveSlotCommitStore &store) noexcept : store_(&store) {}

    /** @copydoc SaveSlotCommitTransaction::Execute */
    Result<SaveSlotCommitResult> SaveSlotCommitTransaction::Execute(OperationId operation, SaveStorageAddress address,
                                                                    std::optional<SaveSlotCatalogEntry> previous,
                                                                    SaveSlotCatalogEntry candidate, ImmutableSaveArchive archive) {
        SaveSlotCommitJournal journal{.operation = operation,
                                      .address = std::move(address),
                                      .previous = std::move(previous),
                                      .candidate = std::move(candidate),
                                      .phase = SaveSlotCommitPhase::Preparing};
        if (auto valid = ValidateJournal(journal); valid.HasError())
            return Result<SaveSlotCommitResult>::Failure(valid.ErrorValue());
        if (!archive.bytes || archive.bytes->empty())
            return Result<SaveSlotCommitResult>::Failure(Invalid("Slot commit requires a non-empty owned finalized archive."));

        if (auto admitted = ValidateAdmission(*store_, journal); admitted.HasError())
            return Result<SaveSlotCommitResult>::Failure(admitted.ErrorValue());
        if (auto prepared = PrepareForPublishing(*store_, journal, archive); prepared.HasError())
            return Result<SaveSlotCommitResult>::Failure(prepared.ErrorValue());
        if (auto published = Publish(*store_, journal); published.HasError())
            return Result<SaveSlotCommitResult>::Failure(published.ErrorValue());
        const auto removed = store_->RemoveJournal(journal);
        return Result<SaveSlotCommitResult>::Success({.cleanupDeferred = removed.HasError()});
    }

    /** @copydoc SaveSlotCommitTransaction::Recover */
    Result<SaveSlotRecoveryAction> SaveSlotCommitTransaction::Recover(const SaveStorageAddress &address) {
        const auto loaded = store_->LoadJournal(address);
        if (loaded.HasError())
            return Result<SaveSlotRecoveryAction>::Failure(
                RecoveryFailure(loaded.ErrorValue(), "Slot recovery journal could not be read."));
        if (!loaded.Value())
            return Result<SaveSlotRecoveryAction>::Success(SaveSlotRecoveryAction::None);

        SaveSlotCommitJournal journal = *loaded.Value();
        if (auto valid = ValidateJournal(journal); valid.HasError())
            return Result<SaveSlotRecoveryAction>::Failure(
                RecoveryFailure(valid.ErrorValue(), "Slot recovery journal is malformed or addresses another authority."));
        if (!Addresses(journal, address))
            return Result<SaveSlotRecoveryAction>::Failure(Invalid("Loaded slot recovery journal does not match the requested address."));

        const auto observed = store_->Observe(journal);
        if (observed.HasError())
            return Result<SaveSlotRecoveryAction>::Failure(
                RecoveryFailure(observed.ErrorValue(), "Slot publication evidence could not be inspected."));
        return RecoverObserved(*store_, std::move(journal), observed.Value());
    }
}  // namespace Horo::Runtime
