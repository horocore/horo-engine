#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime {
    using namespace SaveSlotLifecycleDetail;

    /** @copydoc SaveSlotLifecycle::Cleanup */
    Result<bool> SaveSlotLifecycle::Cleanup() const {
        if (auto selected = VerifySelected(); selected.HasError())
            return Result<bool>::Failure(selected.ErrorValue());
        // Establish selection durability before any journal or last-known-good retirement.
        if (state_->catalog.revision != 1 || !state_->catalog.records.empty() || !state_->catalog.retired.empty()) {
            if (auto durable = state_->storage.SynchronizeLifecycleCatalog(); durable.HasError())
                return Result<bool>::Failure(std::move(durable).ErrorValue());
        }
        auto journal = CleanupJournal();
        if (journal.HasError() || journal.Value())
            return journal;
        return CleanupRetired();
    }

    /** @copydoc SaveSlotLifecycle::VerifySelected */
    Result<void> SaveSlotLifecycle::VerifySelected() const {
        // Never retire last-known-good evidence while a selected generation is missing or damaged.
        // Reconciliation checks existence for every selected record and admits complete selected
        // bytes before retiring any prior generation, including when its journal was already removed.
        for (const auto &record : state_->catalog.records) {
            auto absent = state_->storage.VerifyLifecycleGenerationAbsent(record.entry.publication.generation);
            if (absent.HasValue())
                return Result<void>::Failure(MakeError(SaveErrors::SlotCommitRecoveryFailed));
            if (absent.ErrorValue().code.Value() != SaveErrors::SlotGenerationConflict.code.Value())
                return Result<void>::Failure(absent.ErrorValue());
            if (!state_->catalog.retired.empty()) {
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
    Result<bool> SaveSlotLifecycle::CleanupJournal() const {
        auto journal = state_->storage.ReadLifecycleJournal();
        if (journal.HasError())
            return Result<bool>::Failure(journal.ErrorValue());
        if (journal.Value()) {
            auto ownership = DecodeJournal(*journal.Value(), state_->policy.destination.name);
            if (ownership.HasError())
                return Result<bool>::Failure(ownership.ErrorValue());
            const auto generation = ownership.Value().generation;
            const bool selected = std::ranges::any_of(state_->catalog.records, [&](const Record &record) {
                return record.entry.publication.generation == generation;
            });
            const bool retired = std::ranges::any_of(state_->catalog.retired, [&](const Retired &record) {
                return record.entry.publication.generation == generation;
            });
            auto absent = state_->storage.VerifyLifecycleGenerationAbsent(generation);
            if (absent.HasValue() && (selected || retired))
                return Result<bool>::Failure(MakeError(SaveErrors::SlotCommitRecoveryFailed));
            if (absent.HasError()) {
                if (absent.ErrorValue().code.Value() != SaveErrors::SlotGenerationConflict.code.Value())
                    return Result<bool>::Failure(absent.ErrorValue());
                auto bytes = state_->storage.ReadLifecycleGeneration(generation, state_->policy.archiveLimits.maximumArchiveBytes);
                if (bytes.HasError())
                    return Result<bool>::Failure(bytes.ErrorValue());
                if (ComputeSha256(bytes.Value()) != ownership.Value().bytesHash)
                    return Result<bool>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
            }
            if (!selected && !retired && absent.HasError()) {
                if (auto removed = state_->storage.RemoveLifecycleGeneration(generation); removed.HasError())
                    return Result<bool>::Success(true);
            }
            if (auto removed = state_->storage.RemoveLifecycleJournal(); removed.HasError())
                return Result<bool>::Success(true);
        }
        return Result<bool>::Success(false);
    }

    /** @copydoc SaveSlotLifecycle::CleanupRetired */
    Result<bool> SaveSlotLifecycle::CleanupRetired() const {
        if (state_->catalog.retired.empty())
            return Result<bool>::Success(false);
        bool recycledAny = false;
        for (auto &retired : state_->catalog.retired) {
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
                auto recycled =
                    state_->host->Recycle(retired.entry, {std::make_shared<const std::vector<std::byte>>(std::move(bytes).Value())});
                if (recycled.HasError())
                    return Result<bool>::Success(true);
                retired.recycle = false;
                recycledAny = true;
            }
        }
        // Persist the idempotent platform preservation receipt before physical removal. If a
        // crash precedes this acknowledgement the provider must deduplicate by exact generation.
        if (recycledAny) {
            if (auto published = Publish(); published.HasError())
                return Result<bool>::Success(true);
        }
        for (const auto &retired : state_->catalog.retired) {
            if (auto removed = state_->storage.RemoveLifecycleGeneration(retired.entry.publication.generation); removed.HasError())
                return Result<bool>::Success(true);
        }
        state_->catalog.retired.clear();
        // Cleanup changes no visible slot/revision. If its acknowledgement fails, replaying exact
        // retired-generation deletion is safe; no unrelated artifact is inferred as garbage.
        if (auto published = Publish(); published.HasError())
            return Result<bool>::Success(true);
        return Result<bool>::Success(false);
    }
}  // namespace Horo::Runtime
