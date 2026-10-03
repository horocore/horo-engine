#include "Horo/WorldStreaming/SharedAssetResidency.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Checks every independent charge axis before any ledger mutation. */
        [[nodiscard]] bool Fits(const std::array<std::uint64_t, StreamingBudgetDimensionCount> &charged,
                                const StreamingBudgetAmounts &capacity, const StreamingBudgetAmounts &cost) {
            const auto requests = cost.Entries();
            for (std::size_t index = 0; index < requests.size(); ++index) {
                const auto limit = capacity.Value(requests[index].dimension).Value();
                if (charged[index] > limit || requests[index].value > limit - charged[index])
                    return false;
            }
            return true;
        }

        /** @brief Applies an already-admitted complete charge or its exact retirement reversal. */
        void ApplyCharge(std::array<std::uint64_t, StreamingBudgetDimensionCount> &charged, const StreamingBudgetAmounts &cost,
                         const bool retire) {
            for (const auto &amount : cost.Entries()) {
                const auto index = static_cast<std::size_t>(amount.dimension);
                if (retire)
                    charged[index] -= amount.value;
                else
                    charged[index] += amount.value;
            }
        }

        /** @brief Validates an exact authority-side admission fence before consulting charged entries. */
        [[nodiscard]] Result<void> ValidateAcquireRequest(const SharedAssetResidencyState state, const StreamingRuntimeOwnerToken &owner,
                                                          const std::vector<StreamingFence> &cancelledAttempts, const SharedAssetKey &key,
                                                          const StreamingBudgetAmounts &cost, const SharedAssetConsumer &consumer,
                                                          const StreamingFence &currentAttempt) {
            if (state != SharedAssetResidencyState::Accepting)
                return Failure<void>(WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            if (!key.IsValid() || cost.IsZero() || !consumer.IsValid() || !currentAttempt.IsValid())
                return Failure<void>(WorldStreamingErrors::SharedAssetInvalid);
            if (consumer.fence.partition != owner.partition || consumer.fence.epoch != owner.epoch ||
                currentAttempt.partition != owner.partition || currentAttempt.epoch != owner.epoch || consumer.fence != currentAttempt)
                return Failure<void>(WorldStreamingErrors::SharedAssetStale);
            if (std::ranges::find(cancelledAttempts, consumer.fence) != cancelledAttempts.end())
                return Failure<void>(WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SharedAssetKey::IsValid */
    bool SharedAssetKey::IsValid() const noexcept {
        return asset.IsValid() && revision.IsValid();
    }

    /** @copydoc SharedAssetConsumer::IsValid */
    bool SharedAssetConsumer::IsValid() const noexcept {
        return fence.IsValid() && (!provider || provider->IsValid());
    }

    /** @copydoc SharedAssetLease::IsValid */
    bool SharedAssetLease::IsValid() const noexcept {
        return owner.IsValid() && id.IsValid() && charge.IsValid() && key.IsValid() && consumer.IsValid();
    }

    /** @copydoc SharedAssetRetirement::IsValid */
    bool SharedAssetRetirement::IsValid() const noexcept {
        return owner.IsValid() && charge.IsValid() && key.IsValid();
    }

    /** @copydoc SharedAssetResidencyLimits::IsValid */
    bool SharedAssetResidencyLimits::IsValid() const noexcept {
        return entries > 0 && entries <= MaximumEntries && leases > 0 && leases <= MaximumLeases && !capacity.IsZero();
    }

    SharedAssetResidencyLedger::SharedAssetResidencyLedger(const StreamingRuntimeOwnerToken &owner,
                                                           const SharedAssetResidencyLimits &limits) noexcept
        : owner_(owner), limits_(limits) {}

    /** @copydoc SharedAssetResidencyLedger::SharedAssetResidencyLedger */
    SharedAssetResidencyLedger::SharedAssetResidencyLedger(SharedAssetResidencyLedger &&other) noexcept
        : owner_(other.owner_), limits_(other.limits_), state_(other.state_), charged_(other.charged_),
          nextLeaseValue_(other.nextLeaseValue_), nextChargeValue_(other.nextChargeValue_), entries_(std::move(other.entries_)),
          leases_(std::move(other.leases_)), cancelledAttempts_(std::move(other.cancelledAttempts_)) {
        other.owner_ = {};
        other.state_ = SharedAssetResidencyState::Closed;
        other.charged_ = {};
    }

    /** @copydoc SharedAssetResidencyLedger::Create */
    Result<SharedAssetResidencyLedger> SharedAssetResidencyLedger::Create(const StreamingRuntimeOwnerToken &owner,
                                                                          const SharedAssetResidencyLimits &limits) {
        if (!owner.IsValid() || !limits.IsValid())
            return Failure<SharedAssetResidencyLedger>(WorldStreamingErrors::SharedAssetInvalid);
        SharedAssetResidencyLedger ledger{owner, limits};
        try {
            ledger.entries_.reserve(limits.entries);
            ledger.leases_.reserve(limits.leases);
            ledger.cancelledAttempts_.reserve(limits.leases);
        } catch (const std::bad_alloc &) {
            return Failure<SharedAssetResidencyLedger>(WorldStreamingErrors::SharedAssetCapacityExceeded);
        } catch (const std::length_error &) {
            return Failure<SharedAssetResidencyLedger>(WorldStreamingErrors::SharedAssetCapacityExceeded);
        }
        return Result<SharedAssetResidencyLedger>::Success(std::move(ledger));
    }

    /** @brief Resolves one existing charge or preflights a new charge without mutating either ledger. */
    Result<SharedAssetResidencyLedger::ChargeCandidate> SharedAssetResidencyLedger::PrepareCharge(
        const SharedAssetKey &key, const StreamingBudgetAmounts &residentCost) const {
        if (const auto found = std::ranges::find_if(entries_,
                                                    [&](const Entry &entry) {
            return entry.key == key;
        });
            found != entries_.end()) {
            if (found->cost != residentCost)
                return Failure<ChargeCandidate>(WorldStreamingErrors::SharedAssetConflict);
            if (found->retiring)
                return Failure<ChargeCandidate>(WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            return Result<ChargeCandidate>::Success({.id = found->id, .isNew = false});
        }
        if (entries_.size() >= limits_.entries || nextChargeValue_ == 0 || !Fits(charged_, limits_.capacity, residentCost))
            return Failure<ChargeCandidate>(WorldStreamingErrors::SharedAssetCapacityExceeded);
        return Result<ChargeCandidate>::Success({.id = SharedAssetChargeId::Create(nextChargeValue_).Value(), .isNew = true});
    }

    /** @copydoc SharedAssetResidencyLedger::Acquire */
    Result<SharedAssetLease> SharedAssetResidencyLedger::Acquire(const SharedAssetKey &key, const StreamingBudgetAmounts &residentCost,
                                                                 const SharedAssetConsumer &consumer,
                                                                 const StreamingFence &currentAttempt) {
        if (const auto request = ValidateAcquireRequest(state_, owner_, cancelledAttempts_, key, residentCost, consumer, currentAttempt);
            request.HasError())
            return Result<SharedAssetLease>::Failure(request.ErrorValue());
        if (std::ranges::any_of(leases_, [&](const SharedAssetLease &lease) {
            return lease.key == key && lease.consumer == consumer;
        }))
            return Failure<SharedAssetLease>(WorldStreamingErrors::SharedAssetConflict);
        if (leases_.size() >= limits_.leases || nextLeaseValue_ == 0)
            return Failure<SharedAssetLease>(WorldStreamingErrors::SharedAssetCapacityExceeded);

        const auto charge = PrepareCharge(key, residentCost);
        if (charge.HasError())
            return Result<SharedAssetLease>::Failure(charge.ErrorValue());
        const auto chargeId = charge.Value().id;
        const SharedAssetLease lease{.owner = owner_.owner,
                                     .id = SharedAssetLeaseId::Create(nextLeaseValue_).Value(),
                                     .charge = chargeId,
                                     .key = key,
                                     .consumer = consumer};
        if (charge.Value().isNew) {
            entries_.push_back({.key = key, .id = chargeId, .cost = residentCost, .retiring = false});
            ApplyCharge(charged_, residentCost, false);
            nextChargeValue_ = nextChargeValue_ == std::numeric_limits<std::uint64_t>::max() ? 0 : nextChargeValue_ + 1;
        }
        leases_.push_back(lease);
        nextLeaseValue_ = nextLeaseValue_ == std::numeric_limits<std::uint64_t>::max() ? 0 : nextLeaseValue_ + 1;
        return Result<SharedAssetLease>::Success(lease);
    }

    /** @copydoc SharedAssetResidencyLedger::Release */
    Result<void> SharedAssetResidencyLedger::Release(const SharedAssetLease &lease) {
        if (!lease.IsValid())
            return Failure<void>(WorldStreamingErrors::SharedAssetInvalid);
        const auto found = std::ranges::find_if(leases_, [&](const SharedAssetLease &retained) {
            return retained.id == lease.id;
        });
        if (lease.owner != owner_.owner || found == leases_.end() || *found != lease)
            return Failure<void>(WorldStreamingErrors::SharedAssetStale);
        leases_.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc SharedAssetResidencyLedger::BeginRetire */
    Result<SharedAssetRetirement> SharedAssetResidencyLedger::BeginRetire(const SharedAssetKey &key, const SharedAssetChargeId charge) {
        if (!key.IsValid() || !charge.IsValid())
            return Failure<SharedAssetRetirement>(WorldStreamingErrors::SharedAssetInvalid);
        const auto found = std::ranges::find_if(entries_, [&](const Entry &entry) {
            return entry.key == key;
        });
        if (found == entries_.end() || found->id != charge)
            return Failure<SharedAssetRetirement>(WorldStreamingErrors::SharedAssetStale);
        if (std::ranges::any_of(leases_, [&](const SharedAssetLease &lease) {
            return lease.key == key;
        }))
            return Failure<SharedAssetRetirement>(WorldStreamingErrors::SharedAssetLifecycleUnavailable);
        found->retiring = true;
        return Result<SharedAssetRetirement>::Success({.owner = owner_.owner, .charge = found->id, .key = key});
    }

    /** @copydoc SharedAssetResidencyLedger::AcknowledgeRetired */
    Result<void> SharedAssetResidencyLedger::AcknowledgeRetired(const SharedAssetRetirement &retirement) {
        if (!retirement.IsValid())
            return Failure<void>(WorldStreamingErrors::SharedAssetInvalid);
        const auto found = std::ranges::find_if(entries_, [&](const Entry &entry) {
            return entry.key == retirement.key;
        });
        if (retirement.owner != owner_.owner || found == entries_.end() || found->id != retirement.charge)
            return Failure<void>(WorldStreamingErrors::SharedAssetStale);
        if (!found->retiring)
            return Failure<void>(WorldStreamingErrors::SharedAssetLifecycleUnavailable);
        ApplyCharge(charged_, found->cost, true);
        entries_.erase(found);
        if (state_ == SharedAssetResidencyState::Draining && entries_.empty())
            state_ = SharedAssetResidencyState::Closed;
        return Result<void>::Success();
    }

    /** @copydoc SharedAssetResidencyLedger::CancelAttempt */
    Result<void> SharedAssetResidencyLedger::CancelAttempt(const StreamingFence &fence) {
        if (!fence.IsValid())
            return Failure<void>(WorldStreamingErrors::SharedAssetInvalid);
        if (fence.partition != owner_.partition || fence.epoch != owner_.epoch)
            return Failure<void>(WorldStreamingErrors::SharedAssetStale);
        if (std::ranges::find(cancelledAttempts_, fence) != cancelledAttempts_.end())
            return Result<void>::Success();
        if (cancelledAttempts_.size() >= limits_.leases)
            return Failure<void>(WorldStreamingErrors::SharedAssetCapacityExceeded);
        cancelledAttempts_.push_back(fence);
        return Result<void>::Success();
    }

    /** @copydoc SharedAssetResidencyLedger::BeginShutdown */
    void SharedAssetResidencyLedger::BeginShutdown() noexcept {
        using enum SharedAssetResidencyState;
        if (state_ == Accepting)
            state_ = entries_.empty() ? Closed : Draining;
    }

    /** @copydoc SharedAssetResidencyLedger::Owner */
    const StreamingRuntimeOwnerToken &SharedAssetResidencyLedger::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc SharedAssetResidencyLedger::State */
    SharedAssetResidencyState SharedAssetResidencyLedger::State() const noexcept {
        return state_;
    }

    /** @copydoc SharedAssetResidencyLedger::ChargedCount */
    std::size_t SharedAssetResidencyLedger::ChargedCount() const noexcept {
        return entries_.size();
    }

    /** @copydoc SharedAssetResidencyLedger::Charged */
    Result<std::uint64_t> SharedAssetResidencyLedger::Charged(const StreamingBudgetDimension dimension) const {
        const auto index = static_cast<std::size_t>(dimension);
        if (index >= charged_.size())
            return Failure<std::uint64_t>(WorldStreamingErrors::BudgetDimensionUnsupported);
        return Result<std::uint64_t>::Success(charged_[index]);
    }

    /** @copydoc SharedAssetResidencyLedger::Inspect */
    Result<std::optional<SharedAssetCharge>> SharedAssetResidencyLedger::Inspect(const SharedAssetKey &key) const {
        if (!key.IsValid())
            return Failure<std::optional<SharedAssetCharge>>(WorldStreamingErrors::SharedAssetInvalid);
        const auto found = std::ranges::find(entries_, key, &Entry::key);
        if (found == entries_.end())
            return Result<std::optional<SharedAssetCharge>>::Success(std::nullopt);
        return Result<std::optional<SharedAssetCharge>>::Success(SharedAssetCharge{found->id, found->cost, found->retiring});
    }

    /** @copydoc SharedAssetResidencyLedger::LeaseCount */
    std::size_t SharedAssetResidencyLedger::LeaseCount() const noexcept {
        return leases_.size();
    }
}  // namespace Horo::WorldStreaming
