#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime {
    using namespace SaveSlotLifecycleDetail;

    /** @copydoc SaveSlotLifecycle::CleanupAfterPublication */
    bool SaveSlotLifecycle::CleanupAfterPublication(Operation &operation) const noexcept {
        // The selection is already durable. Any unexpected host/observer exception must leave
        // the original publication successful, with recovery evidence for later worker cleanup.
        try {
            auto cleaned = Cleanup(operation);
            return cleaned.HasError() || cleaned.Value();
        } catch (const std::bad_alloc &) {
            return true;
        } catch (...) {
            return true;
        }
    }

    /** @copydoc SaveSlotLifecycle::Cleanup */
    Result<bool> SaveSlotLifecycle::Cleanup(Operation &operation) const {
        if (auto selected = VerifySelected(operation); selected.HasError())
            return Result<bool>::Failure(selected.ErrorValue());
        // Establish selection durability before any journal or last-known-good retirement.
        if (operation.catalog.revision != 1 || !operation.catalog.records.empty() || !operation.catalog.retired.empty()) {
            if (auto durable = state_->storage.SynchronizeLifecycleCatalog(); durable.HasError())
                return Result<bool>::Failure(std::move(durable).ErrorValue());
        }
        if (auto journal = CleanupJournal(operation); journal.HasError() || journal.Value())
            return journal;
        return CleanupRetired(operation);
    }

    /** @copydoc SaveSlotLifecycle::VerifySelected */
    Result<void> SaveSlotLifecycle::VerifySelected(const Operation &operation) const {
        // Never retire last-known-good evidence while a selected generation is missing or damaged.
        // Reconciliation checks existence for every selected record and admits complete selected
        // bytes before retiring any prior generation, including when its journal was already removed.
        for (const auto &record : operation.catalog.records) {
            auto absent = state_->storage.VerifyLifecycleGenerationAbsent(record.entry.publication.generation);
            if (absent.HasValue())
                return Result<void>::Failure(MakeError(SaveErrors::SlotCommitRecoveryFailed));
            if (absent.ErrorValue().code.Value() != SaveErrors::SlotGenerationConflict.code.Value())
                return Result<void>::Failure(absent.ErrorValue());
            if (!operation.catalog.retired.empty()) {
                auto bytes = state_->storage.ReadLifecycleGeneration(record.entry.publication.generation,
                                                                     state_->policy.archiveLimits.maximumArchiveBytes);
                if (bytes.HasError())
                    return Result<void>::Failure(bytes.ErrorValue());
                auto archive = Admit(std::move(bytes).Value(), state_->policy.destination, state_->policy, *state_->host);
                if (archive.HasError())
                    return Result<void>::Failure(archive.ErrorValue());
                if (auto matched = Matches(record.entry, archive.Value()); matched.HasError())
                    return Result<void>::Failure(matched.ErrorValue());
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc SaveSlotLifecycle::CleanupJournal */
    Result<bool> SaveSlotLifecycle::CleanupJournal(const Operation &operation) const {
        auto journal = state_->storage.ReadLifecycleJournal();
        if (journal.HasError())
            return Result<bool>::Failure(journal.ErrorValue());
        if (journal.Value()) {
            auto ownership = DecodeJournal(*journal.Value(), state_->policy.destination.name);
            if (ownership.HasError())
                return Result<bool>::Failure(ownership.ErrorValue());
            const auto generation = ownership.Value().generation;
            const bool selected = std::ranges::any_of(operation.catalog.records, [generation](const Record &record) {
                return record.entry.publication.generation == generation;
            });
            const bool retired = std::ranges::any_of(operation.catalog.retired, [generation](const Retired &record) {
                return record.entry.publication.generation == generation;
            });
            auto present = CheckJournalGeneration(generation, ownership.Value().bytesHash, selected || retired);
            if (present.HasError())
                return Result<bool>::Failure(present.ErrorValue());
            if (!selected && !retired && present.Value()) {
                if (auto removed = state_->storage.RemoveLifecycleGeneration(generation); removed.HasError())
                    return Result<bool>::Success(true);
            }
            if (auto removed = state_->storage.RemoveLifecycleJournal(); removed.HasError())
                return Result<bool>::Success(true);
        }
        return Result<bool>::Success(false);
    }

    /** @copydoc SaveSlotLifecycle::CheckJournalGeneration */
    Result<bool> SaveSlotLifecycle::CheckJournalGeneration(const SlotGenerationId generation, const Sha256Digest &bytesHash,
                                                           const bool required) const {
        auto absent = state_->storage.VerifyLifecycleGenerationAbsent(generation);
        if (absent.HasValue()) {
            if (required)
                return Result<bool>::Failure(MakeError(SaveErrors::SlotCommitRecoveryFailed));
            return Result<bool>::Success(false);
        }
        if (absent.ErrorValue().code.Value() != SaveErrors::SlotGenerationConflict.code.Value())
            return Result<bool>::Failure(absent.ErrorValue());
        auto bytes = state_->storage.ReadLifecycleGeneration(generation, state_->policy.archiveLimits.maximumArchiveBytes);
        if (bytes.HasError())
            return Result<bool>::Failure(bytes.ErrorValue());
        if (ComputeSha256(bytes.Value()) != bytesHash)
            return Result<bool>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        return Result<bool>::Success(true);
    }

    /** @copydoc SaveSlotLifecycle::PreserveRetired */
    Result<bool> SaveSlotLifecycle::PreserveRetired(Operation &operation) const {
        bool recycledAny = false;
        for (auto &retired : operation.catalog.retired) {
            if (retired.recycle) {
                if (!state_->policy.allowPlatformRecycle || !state_->host->SupportsRecycle())
                    return Result<bool>::Success(true);
                auto bytes = state_->storage.ReadLifecycleGeneration(retired.entry.publication.generation,
                                                                     state_->policy.archiveLimits.maximumArchiveBytes);
                if (bytes.HasError())
                    return Result<bool>::Success(true);
                auto archive = Admit(bytes.Value(), state_->policy.destination, state_->policy, *state_->host);
                if (archive.HasError())
                    return Result<bool>::Failure(archive.ErrorValue());
                if (auto matched = Matches(retired.entry, archive.Value()); matched.HasError())
                    return Result<bool>::Failure(matched.ErrorValue());
                if (auto recycled =
                        state_->host->Recycle(retired.entry, {std::make_shared<const std::vector<std::byte>>(std::move(bytes).Value())});
                    recycled.HasError())
                    return Result<bool>::Success(true);
                retired.recycle = false;
                recycledAny = true;
            }
        }
        // Persist the idempotent platform preservation receipt before physical removal. If a
        // crash precedes this acknowledgement the provider must deduplicate by exact generation.
        if (recycledAny) {
            if (auto published = Publish(operation); published.HasError())
                return Result<bool>::Success(true);
        }
        return Result<bool>::Success(false);
    }

    /** @copydoc SaveSlotLifecycle::CleanupRetired */
    Result<bool> SaveSlotLifecycle::CleanupRetired(Operation &operation) const {
        if (operation.catalog.retired.empty())
            return Result<bool>::Success(false);
        if (auto preserved = PreserveRetired(operation); preserved.HasError() || preserved.Value())
            return preserved;
        bool removedAny = false;
        for (const auto &retired : operation.catalog.retired) {
            if (retired.backup || retired.tombstone)
                continue;
            removedAny = true;
            if (auto removed = state_->storage.RemoveLifecycleGeneration(retired.entry.publication.generation); removed.HasError())
                return Result<bool>::Success(true);
        }
        if (!removedAny)
            return Result<bool>::Success(false);
        std::erase_if(operation.catalog.retired, [](const Retired &record) {
            return !record.backup && !record.tombstone;
        });
        // Cleanup changes no visible slot/revision. If its acknowledgement fails, replaying exact
        // retired-generation deletion is safe; no unrelated artifact is inferred as garbage.
        if (auto published = Publish(operation); published.HasError())
            return Result<bool>::Success(true);
        return Result<bool>::Success(false);
    }
}  // namespace Horo::Runtime
