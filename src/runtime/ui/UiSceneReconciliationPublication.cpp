#include "UiSceneReconciliationInternal.h"

#include <limits>

namespace Horo::Runtime::Ui {
    using UiSceneDetail::Failure;

    /** @copydoc UiSceneReconciliation::CanCommit */
    Result<void> UiSceneReconciliation::CanCommit(const Prepared &prepared, const UiStructuralCommitPoint point) const {
        if (!storage_ || storage_->stopped || storage_->collecting || !prepared.storage_ || prepared.storage_->consumed ||
            !UiSceneDetail::Cutoff(point))
            return Failure(UiErrors::InstanceStateInvalid);
        const auto &candidate = *prepared.storage_;
        if (candidate.owner != storage_ || candidate.revision != storage_->revision)
            return Failure(UiErrors::RevisionStale);
        if (storage_->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure(UiErrors::GenerationExhausted);
        if (candidate.cancellation.IsCancellationRequested())
            return Failure(UiErrors::AssetLoadCancelled);
        if (candidate.retiring.size() + storage_->limits.maximumInstances > storage_->FreeRetired())
            return Failure(UiErrors::CapacityExceeded);
        for (const auto &retiring : candidate.retiring) {
            const auto &entry = storage_->active[retiring.slot];
            if (!entry || !entry->publisher.IsCurrent(retiring.source) ||
                !UiReloadDetail::MatchesSource(*retiring.source.Get(), retiring.stamps))
                return Failure(UiErrors::RevisionStale);
        }
        for (const auto &rebind : candidate.rebinding) {
            const auto &entry = storage_->active[rebind.slot];
            if (!entry)
                return Failure(UiErrors::HandleStale);
            if (const auto valid = entry->publisher.CanCommit(rebind.candidate, point); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Commit */
    Result<UiSceneReconciliationResult> UiSceneReconciliation::Commit(Prepared &prepared, const UiStructuralCommitPoint point) {
        if (const auto valid = CanCommit(prepared, point); valid.HasError())
            return Result<UiSceneReconciliationResult>::Failure(valid.ErrorValue());
        auto &candidate = *prepared.storage_;
        for (auto &rebind : candidate.rebinding) {
            auto &entry = *storage_->active[rebind.slot];
            entry.publisher.CommitValidated(rebind.candidate);
            // Unload leaves explicit unavailable/fallback source evidence; ownership remains persistent.
            entry.descriptor.providerScene = candidate.request.next;
        }
        for (const auto &retiring : candidate.retiring) {
            auto &entry = storage_->active[retiring.slot];
            entry->publisher.Shutdown();
            const auto free = std::ranges::find_if(storage_->retired, [](const auto &value) {
                return !value;
            });
            free->emplace(std::move(*entry));
            entry.reset();
        }
        for (auto &incoming : candidate.incoming) {
            const auto free = std::ranges::find_if(storage_->active, [](const auto &entry) {
                return !entry;
            });
            free->emplace(std::move(incoming));
        }
        candidate.incoming.clear();
        candidate.consumed = true;
        ++storage_->revision;
        return Result<UiSceneReconciliationResult>::Success(candidate.result);
    }

    /** @copydoc UiSceneReconciliation::CollectRetired */
    Result<std::size_t> UiSceneReconciliation::CollectRetired() {
        if (!storage_ || storage_->collecting)
            return Failure<std::size_t>(UiErrors::InstanceStateInvalid);
        const auto owner = storage_;
        owner->collecting = true;

        struct Guard final {
            Storage &owner;

            ~Guard() {
                owner.collecting = false;
                if (owner.shutdownRequested)
                    owner.Stop();
            }
        } guard{*owner};

        for (auto &entry : owner->active)
            if (entry) {
                const auto drained = entry->publisher.CollectRetired();
                if (drained.HasError())
                    return Result<std::size_t>::Failure(drained.ErrorValue());
                if (owner->shutdownRequested)
                    return Result<std::size_t>::Success(0);
            }
        std::size_t reclaimed = 0;
        for (auto &entry : owner->retired)
            if (entry) {
                const auto drained = entry->publisher.CollectRetired();
                if (drained.HasError())
                    return Result<std::size_t>::Failure(drained.ErrorValue());
                if (owner->shutdownRequested)
                    return Result<std::size_t>::Success(reclaimed);
                if (entry->publisher.CanReclaim()) {
                    entry.reset();
                    ++reclaimed;
                }
            }
        return Result<std::size_t>::Success(reclaimed);
    }

    /** @brief Stops all semantic owners after any executing retirement callback has unwound. */
    void UiSceneReconciliation::Storage::Stop() noexcept {
        if (stopped)
            return;
        stopped = true;
        shutdownRequested = false;
        for (auto &entry : active)
            if (entry) {
                entry->publisher.Shutdown();
                const auto free = std::ranges::find_if(retired, [](const auto &value) {
                    return !value;
                });
                free->emplace(std::move(*entry));
                entry.reset();
            }
    }

    /** @copydoc UiSceneReconciliation::Shutdown */
    void UiSceneReconciliation::Shutdown() noexcept {
        if (!storage_ || storage_->stopped)
            return;
        if (storage_->collecting) {
            storage_->shutdownRequested = true;
            return;
        }
        storage_->Stop();
    }

    /** @copydoc UiSceneReconciliation::CanReclaim */
    bool UiSceneReconciliation::CanReclaim() const noexcept {
        return !storage_ || (storage_->stopped && !storage_->collecting && storage_->preparedCount == 0 &&
                             std::ranges::all_of(storage_->retired, [](const auto &entry) {
            return !entry || entry->publisher.CanReclaim();
        }));
    }
}  // namespace Horo::Runtime::Ui
