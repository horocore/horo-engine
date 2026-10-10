#include "UiSceneReconciliationInternal.h"

#include <limits>

namespace Horo::Runtime::Ui {
    using UiSceneDetail::Failure;

    /** @copydoc UiSceneReconciliation::CanCommit */
    Result<void> UiSceneReconciliation::CanCommit(const Prepared &prepared, const UiStructuralCommitPoint point) const {
        if (!storage_ || storage_.Get()->stopped || storage_.Get()->collecting || !prepared.storage_ || prepared.storage_->consumed ||
            !UiSceneDetail::Cutoff(point))
            return Failure(UiErrors::InstanceStateInvalid);
        const auto &candidate = *prepared.storage_;
        if (!storage_.Matches(candidate.owner) || candidate.revision != storage_.Get()->revision)
            return Failure(UiErrors::RevisionStale);
        if (storage_.Get()->revision == std::numeric_limits<std::uint64_t>::max())
            return Failure(UiErrors::GenerationExhausted);
        if (candidate.cancellation.IsCancellationRequested())
            return Failure(UiErrors::AssetLoadCancelled);
        if (candidate.retiring.size() + storage_.Get()->limits.maximumInstances > storage_.Get()->FreeRetired())
            return Failure(UiErrors::CapacityExceeded);
        for (const auto &retiring : candidate.retiring) {
            const auto &entry = storage_.Get()->active[retiring.slot];
            if (!entry || !entry->publisher.IsCurrent(retiring.source) ||
                !UiReloadDetail::MatchesSource(*retiring.source.Get(), retiring.stamps))
                return Failure(UiErrors::RevisionStale);
        }
        for (const auto &rebind : candidate.rebinding) {
            const auto &entry = storage_.Get()->active[rebind.slot];
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
            auto &entry = *storage_.Get()->active[rebind.slot];
            entry.publisher.CommitValidated(rebind.candidate);
            // Unload leaves explicit unavailable/fallback source evidence; ownership remains persistent.
            entry.descriptor.providerScene = candidate.request.next;
        }
        for (const auto &retiring : candidate.retiring) {
            auto &entry = storage_.Get()->active[retiring.slot];
            entry->publisher.Shutdown();
            const auto free = std::ranges::find_if(storage_.Get()->retired, [](const auto &value) {
                return !value;
            });
            free->emplace(std::move(*entry));
            entry.reset();
        }
        for (auto &incoming : candidate.incoming) {
            const auto free = std::ranges::find_if(storage_.Get()->active, [](const auto &entry) {
                return !entry;
            });
            free->emplace(std::move(incoming));
        }
        candidate.incoming.clear();
        candidate.consumed = true;
        ++storage_.Get()->revision;
        return Result<UiSceneReconciliationResult>::Success(candidate.result);
    }

    /** @copydoc UiSceneReconciliation::CollectRetired */
    Result<std::size_t> UiSceneReconciliation::CollectRetired() {
        if (!storage_ || storage_.Get()->collecting)
            return Failure<std::size_t>(UiErrors::InstanceStateInvalid);
        const auto owner = storage_.OwnerPin();
        owner->collecting = true;

        struct Guard final {
            explicit Guard(Storage &state) noexcept : owner(state) {}

            Guard(const Guard &) = delete;
            Guard &operator=(const Guard &) = delete;
            Guard(Guard &&) = delete;
            Guard &operator=(Guard &&) = delete;
            Storage &owner;

            ~Guard() {
                owner.collecting = false;
                if (owner.shutdownRequested)
                    owner.Stop();
            }
        };

        Guard guard{*owner};

        for (auto &entry : owner->active)
            if (entry) {
                if (const auto drained = entry->publisher.CollectRetired(); drained.HasError())
                    return Result<std::size_t>::Failure(drained.ErrorValue());
                if (owner->shutdownRequested)
                    return Result<std::size_t>::Success(0);
            }
        std::size_t reclaimed = 0;
        for (auto &entry : owner->retired)
            if (entry) {
                if (const auto drained = entry->publisher.CollectRetired(); drained.HasError())
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
        if (!storage_ || storage_.Get()->stopped)
            return;
        if (storage_.Get()->collecting) {
            storage_.Get()->shutdownRequested = true;
            return;
        }
        storage_.Get()->Stop();
    }

    /** @copydoc UiSceneReconciliation::CanReclaim */
    bool UiSceneReconciliation::CanReclaim() const noexcept {
        return !storage_ || (storage_.Get()->stopped && !storage_.Get()->collecting && storage_.Get()->preparedCount == 0 &&
                             std::ranges::all_of(storage_.Get()->retired, [](const auto &entry) {
            return !entry || entry->publisher.CanReclaim();
        }));
    }
}  // namespace Horo::Runtime::Ui
