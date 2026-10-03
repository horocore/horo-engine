#include "Horo/WorldStreaming/StreamingFeatureBudgetReservations.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using Internal::Failure;
        using namespace WorldStreamingErrors;

        /** @brief Validates transfer bounds before either owned ledger can publish a charge. */
        [[nodiscard]] Result<void> ValidateTransfer(const bool isNew, const StreamingBudgetAmounts &cost,
                                                    const StreamingBudgetAmounts &portion,
                                                    const std::array<std::uint64_t, StreamingBudgetDimensionCount> &remaining) {
            if (isNew && portion != cost)
                return Failure<void>(FeatureBudgetInvalid);
            const auto peak = portion.Entries();
            const auto resident = cost.Entries();
            for (std::size_t axis = 0; axis < peak.size(); ++axis)
                if (peak[axis].value > remaining[axis] || peak[axis].value > resident[axis].value)
                    return Failure<void>(FeatureBudgetCapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Consumes an admitted peak; only cache reuse returns its duplicate aggregate credit. */
        void ConsumePeak(std::array<std::uint64_t, StreamingBudgetDimensionCount> &remaining,
                         std::array<std::uint64_t, StreamingBudgetDimensionCount> &used, const StreamingBudgetAmounts &portion,
                         const bool isNew) {
            for (const auto &amount : portion.Entries()) {
                const auto axis = static_cast<std::size_t>(amount.dimension);
                remaining[axis] -= amount.value;
                if (!isNew)
                    used[axis] -= amount.value;
            }
        }
    }  // namespace

    StreamingFeatureBudgetReservations::StreamingFeatureBudgetReservations(WorldStreamingRuntimeComposition runtime,
                                                                           SharedAssetResidencyLedger shared,
                                                                           const StreamingFeatureBudgetPolicy &policy) noexcept
        : runtime_(std::move(runtime)), shared_(std::move(shared)), policy_(policy) {}

    /** @copydoc StreamingFeatureBudgetReservations::Create */
    Result<StreamingFeatureBudgetReservations> StreamingFeatureBudgetReservations::Create(
        const StreamingFeatureBudgetConfig &config, const std::span<const StreamingRuntimeServiceBinding> services,
        const StreamingFeatureBudgetPolicy &policy) {
        if (config.contractVersion != StreamingFeatureBudgetConfig::CurrentContractVersion)
            return Failure<StreamingFeatureBudgetReservations>(FeatureBudgetUnsupported);
        auto runtime = WorldStreamingRuntimeComposition::Create(config.runtime, services);
        if (runtime.HasError())
            return Result<StreamingFeatureBudgetReservations>::Failure(runtime.ErrorValue());
        auto shared =
            SharedAssetResidencyLedger::Create(config.runtime.owner, {config.sharedEntries, config.sharedLeases, policy.GlobalCapacity()});
        if (shared.HasError())
            return Result<StreamingFeatureBudgetReservations>::Failure(shared.ErrorValue());
        StreamingFeatureBudgetReservations ledger{std::move(runtime).Value(), std::move(shared).Value(), policy};
        try {
            ledger.entries_.reserve(config.runtime.schedulerLimits.concurrentOperations);
            ledger.chargeSlices_.reserve(config.sharedEntries);
        } catch (const std::bad_alloc &) {
            return Failure<StreamingFeatureBudgetReservations>(FeatureBudgetCapacityExceeded);
        } catch (const std::length_error &) {
            return Failure<StreamingFeatureBudgetReservations>(FeatureBudgetCapacityExceeded);
        }
        return Result<StreamingFeatureBudgetReservations>::Success(std::move(ledger));
    }

    Result<void> StreamingFeatureBudgetReservations::Validate(const StreamingFeatureBudgetContext &context) const {
        if (!context.owner.IsValid() || !context.policyRevision.IsValid() || !context.revision.IsValid())
            return Failure<void>(FeatureBudgetInvalid);
        if (context.owner != runtime_.Owner() || context.policyRevision != policy_.Revision() || context.revision != revision_)
            return Failure<void>(FeatureBudgetStale);
        if (revision_.Value() == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(GenerationExhausted);
        return Result<void>::Success();
    }

    Result<std::size_t> StreamingFeatureBudgetReservations::Find(const StreamingFeatureBudgetReservation &reservation,
                                                                 const bool requireActive) const {
        if (!reservation.IsValid())
            return Failure<std::size_t>(FeatureBudgetInvalid);
        const auto found = std::ranges::find(entries_, reservation, &Entry::reservation);
        if (reservation.owner != runtime_.Owner() || found == entries_.end())
            return Failure<std::size_t>(FeatureBudgetStale);
        if (requireActive) {
            const auto operation = runtime_.Scheduler().Inspect(reservation.scheduler);
            if (operation.HasError())
                return Result<std::size_t>::Failure(operation.ErrorValue());
            using enum StreamingCellOperationState;
            if (State() != StreamingSchedulerAdmissionState::Accepting || operation.Value().State() == Retiring ||
                operation.Value().IsTerminal() || operation.Value().Kind() == StreamingCellOperationKind::Retire)
                return Failure<std::size_t>(FeatureBudgetLifecycleUnavailable);
        }
        return Result<std::size_t>::Success(static_cast<std::size_t>(found - entries_.begin()));
    }

    bool StreamingFeatureBudgetReservations::Fits(const Matrix &additional) const noexcept {
        for (std::size_t axis = 0; axis < StreamingBudgetDimensionCount; ++axis) {
            auto available = policy_.global_[axis];
            for (std::size_t feature = 0; feature < StreamingBudgetFeatureCount; ++feature) {
                if (const auto limit = policy_.capacity_.values_[feature][axis];
                    used_[feature][axis] > limit || additional[feature][axis] > limit - used_[feature][axis])
                    return false;
                if (used_[feature][axis] > available)
                    return false;
                available -= used_[feature][axis];
            }
            for (const auto &feature : additional) {
                if (feature[axis] > available)
                    return false;
                available -= feature[axis];
            }
        }
        return true;
    }

    void StreamingFeatureBudgetReservations::Apply(const Matrix &amounts, const bool release) noexcept {
        for (std::size_t feature = 0; feature < StreamingBudgetFeatureCount; ++feature)
            for (std::size_t axis = 0; axis < StreamingBudgetDimensionCount; ++axis)
                if (release)
                    used_[feature][axis] -= amounts[feature][axis];
                else
                    used_[feature][axis] += amounts[feature][axis];
    }

    void StreamingFeatureBudgetReservations::Publish() noexcept {
        revision_ = StreamingFeatureBudgetRevision::Create(revision_.Value() + 1).Value();
    }

    /** @copydoc StreamingFeatureBudgetReservations::TryAdmit */
    Result<StreamingFeatureBudgetReservation> StreamingFeatureBudgetReservations::TryAdmit(const StreamingFeatureBudgetContext &context,
                                                                                           const StreamingCellOperation &operation,
                                                                                           const StreamingFeatureBudgetPlan &peak,
                                                                                           const std::uint64_t capacityUnits,
                                                                                           const StreamingFence &currentAttempt) {
        if (const auto valid = Validate(context); valid.HasError())
            return Result<StreamingFeatureBudgetReservation>::Failure(valid.ErrorValue());
        if (!currentAttempt.IsValid())
            return Failure<StreamingFeatureBudgetReservation>(FeatureBudgetInvalid);
        if (currentAttempt.partition != runtime_.Owner().partition || currentAttempt.epoch != runtime_.Owner().epoch ||
            currentAttempt != operation.Handle().fence)
            return Failure<StreamingFeatureBudgetReservation>(FeatureBudgetStale);
        if (!Fits(peak.values_))
            return Failure<StreamingFeatureBudgetReservation>(FeatureBudgetCapacityExceeded);
        auto &scheduler = runtime_.Scheduler();
        const auto admitted = scheduler.TryAdmit(operation, capacityUnits, scheduler.Limits().concurrency.revision);
        if (admitted.HasError())
            return Result<StreamingFeatureBudgetReservation>::Failure(admitted.ErrorValue());
        const StreamingFeatureBudgetReservation reservation{runtime_.Owner(), admitted.Value()};
        entries_.emplace_back(reservation, peak.values_);
        Apply(peak.values_, false);
        Publish();
        return Result<StreamingFeatureBudgetReservation>::Success(reservation);
    }

    /** @copydoc StreamingFeatureBudgetReservations::Grow */
    Result<void> StreamingFeatureBudgetReservations::Grow(const StreamingFeatureBudgetContext &context,
                                                          const StreamingFeatureBudgetReservation &reservation,
                                                          const StreamingFeatureBudgetPlan &additional) {
        if (const auto valid = Validate(context); valid.HasError())
            return valid;
        const auto index = Find(reservation, true);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        if (!Fits(additional.values_))
            return Failure<void>(FeatureBudgetCapacityExceeded);
        auto &remaining = entries_[index.Value()].remaining;
        for (std::size_t feature = 0; feature < StreamingBudgetFeatureCount; ++feature)
            for (std::size_t axis = 0; axis < StreamingBudgetDimensionCount; ++axis)
                remaining[feature][axis] += additional.values_[feature][axis];
        Apply(additional.values_, false);
        Publish();
        return Result<void>::Success();
    }

    /** @copydoc StreamingFeatureBudgetReservations::Advance */
    Result<StreamingCellOperation> StreamingFeatureBudgetReservations::Advance(const StreamingFeatureBudgetContext &context,
                                                                               const StreamingFeatureBudgetReservation &reservation,
                                                                               const StreamingCellOperationTransition transition) {
        if (const auto valid = Validate(context); valid.HasError())
            return Result<StreamingCellOperation>::Failure(valid.ErrorValue());
        if (const auto index = Find(reservation); index.HasError())
            return Result<StreamingCellOperation>::Failure(index.ErrorValue());
        const auto advanced = runtime_.Scheduler().Advance(reservation.scheduler, transition);
        if (advanced.HasValue())
            Publish();
        return advanced;
    }

    /** @copydoc StreamingFeatureBudgetReservations::Release */
    Result<void> StreamingFeatureBudgetReservations::Release(const StreamingFeatureBudgetContext &context,
                                                             const StreamingFeatureBudgetReservation &reservation) {
        if (const auto valid = Validate(context); valid.HasError())
            return valid;
        const auto index = Find(reservation);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        if (const auto released = runtime_.Scheduler().Release(reservation.scheduler); released.HasError())
            return released;
        Apply(entries_[index.Value()].remaining, true);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index.Value()));
        Publish();
        return Result<void>::Success();
    }

    /** @copydoc StreamingFeatureBudgetReservations::RealizeShared */
    Result<SharedAssetLease> StreamingFeatureBudgetReservations::RealizeShared(const StreamingFeatureBudgetContext &context,
                                                                               const StreamingFeatureBudgetReservation &reservation,
                                                                               const StreamingSharedAssetRealization &realization) {
        const auto &[feature, key, residentCost, peakPortion, consumer] = realization;
        if (const auto valid = Validate(context); valid.HasError())
            return Result<SharedAssetLease>::Failure(valid.ErrorValue());
        const auto featureIndex = static_cast<std::size_t>(feature);
        if (featureIndex >= StreamingBudgetFeatureCount)
            return Failure<SharedAssetLease>(FeatureBudgetUnsupported);
        const auto index = Find(reservation, true);
        if (index.HasError())
            return Result<SharedAssetLease>::Failure(index.ErrorValue());
        if (consumer.fence != reservation.scheduler.operation.fence)
            return Failure<SharedAssetLease>(FeatureBudgetStale);
        const auto existing = shared_.Inspect(key);
        if (existing.HasError())
            return Result<SharedAssetLease>::Failure(existing.ErrorValue());
        const bool isNew = !existing.Value();
        auto &remaining = entries_[index.Value()].remaining[featureIndex];
        if (const auto valid = ValidateTransfer(isNew, residentCost, peakPortion, remaining); valid.HasError())
            return Result<SharedAssetLease>::Failure(valid.ErrorValue());
        const auto lease = shared_.Acquire(key, residentCost, consumer, reservation.scheduler.operation.fence);
        if (lease.HasError())
            return lease;
        if (isNew)
            chargeSlices_.push_back({lease.Value().charge, feature});
        ConsumePeak(remaining, used_[featureIndex], peakPortion, isNew);
        Publish();
        return lease;
    }

    /** @copydoc StreamingFeatureBudgetReservations::ReleaseShared */
    Result<void> StreamingFeatureBudgetReservations::ReleaseShared(const StreamingFeatureBudgetContext &context,
                                                                   const SharedAssetLease &lease) {
        if (const auto valid = Validate(context); valid.HasError())
            return valid;
        const auto released = shared_.Release(lease);
        if (released.HasValue())
            Publish();
        return released;
    }

    /** @copydoc StreamingFeatureBudgetReservations::BeginRetireShared */
    Result<SharedAssetRetirement> StreamingFeatureBudgetReservations::BeginRetireShared(const StreamingFeatureBudgetContext &context,
                                                                                        const SharedAssetKey &key,
                                                                                        const SharedAssetChargeId charge) {
        if (const auto valid = Validate(context); valid.HasError())
            return Result<SharedAssetRetirement>::Failure(valid.ErrorValue());
        const auto retirement = shared_.BeginRetire(key, charge);
        if (retirement.HasValue())
            Publish();
        return retirement;
    }

    /** @copydoc StreamingFeatureBudgetReservations::AcknowledgeSharedRetired */
    Result<void> StreamingFeatureBudgetReservations::AcknowledgeSharedRetired(const StreamingFeatureBudgetContext &context,
                                                                              const SharedAssetRetirement &retirement) {
        if (const auto valid = Validate(context); valid.HasError())
            return valid;
        const auto charge = shared_.Inspect(retirement.key);
        if (charge.HasError())
            return Result<void>::Failure(charge.ErrorValue());
        const auto slice = std::ranges::find(chargeSlices_, retirement.charge, &ChargeSlice::charge);
        if (!charge.Value() || charge.Value()->id != retirement.charge || slice == chargeSlices_.end())
            return Failure<void>(FeatureBudgetStale);
        if (const auto retired = shared_.AcknowledgeRetired(retirement); retired.HasError())
            return retired;
        const auto feature = static_cast<std::size_t>(slice->feature);
        for (const auto &amount : charge.Value()->cost.Entries())
            used_[feature][static_cast<std::size_t>(amount.dimension)] -= amount.value;
        chargeSlices_.erase(slice);
        Publish();
        return Result<void>::Success();
    }

    /** @copydoc StreamingFeatureBudgetReservations::ReplaceServices */
    Result<void> StreamingFeatureBudgetReservations::ReplaceServices(const StreamingFeatureBudgetContext &context,
                                                                     const StreamingRuntimeCompositionRevision revision,
                                                                     const std::span<const StreamingRuntimeServiceBinding> services) {
        if (const auto valid = Validate(context); valid.HasError())
            return valid;
        if (shared_.ChargedCount() != 0)
            return Failure<void>(FeatureBudgetLifecycleUnavailable);
        const auto replaced = runtime_.Replace(runtime_.Owner(), revision, services);
        if (replaced.HasValue())
            Publish();
        return replaced;
    }

    /** @copydoc StreamingFeatureBudgetReservations::BeginShutdown */
    Result<void> StreamingFeatureBudgetReservations::BeginShutdown(const StreamingFeatureBudgetContext &context) {
        if (const auto valid = Validate(context); valid.HasError())
            return valid;
        if (State() != StreamingSchedulerAdmissionState::Accepting)
            return Result<void>::Success();
        if (const auto shutdown = runtime_.BeginShutdown(runtime_.Owner()); shutdown.HasError())
            return shutdown;
        shared_.BeginShutdown();
        Publish();
        return Result<void>::Success();
    }

    /** @copydoc StreamingFeatureBudgetReservations::Context */
    StreamingFeatureBudgetContext StreamingFeatureBudgetReservations::Context() const noexcept {
        return {runtime_.Owner(), policy_.Revision(), revision_};
    }

    /** @copydoc StreamingFeatureBudgetReservations::State */
    StreamingSchedulerAdmissionState StreamingFeatureBudgetReservations::State() const noexcept {
        using enum StreamingSchedulerAdmissionState;
        if (runtime_.State() == WorldStreamingRuntimeCompositionState::Active)
            return Accepting;
        return runtime_.State() == WorldStreamingRuntimeCompositionState::Closed && shared_.State() == SharedAssetResidencyState::Closed
                   ? Closed
                   : Draining;
    }

    /** @copydoc StreamingFeatureBudgetReservations::Used(StreamingBudgetFeature, StreamingBudgetDimension) const */
    Result<std::uint64_t> StreamingFeatureBudgetReservations::Used(const StreamingBudgetFeature feature,
                                                                   const StreamingBudgetDimension dimension) const {
        const auto index = static_cast<std::size_t>(feature);
        const auto axis = static_cast<std::size_t>(dimension);
        if (index >= StreamingBudgetFeatureCount)
            return Failure<std::uint64_t>(FeatureBudgetUnsupported);
        if (axis >= StreamingBudgetDimensionCount)
            return Failure<std::uint64_t>(BudgetDimensionUnsupported);
        return Result<std::uint64_t>::Success(used_[index][axis]);
    }

    /** @copydoc StreamingFeatureBudgetReservations::Used(StreamingBudgetDimension) const */
    Result<std::uint64_t> StreamingFeatureBudgetReservations::Used(const StreamingBudgetDimension dimension) const {
        const auto axis = static_cast<std::size_t>(dimension);
        if (axis >= StreamingBudgetDimensionCount)
            return Failure<std::uint64_t>(BudgetDimensionUnsupported);
        std::uint64_t sum{};
        for (const auto &feature : used_)
            sum += feature[axis];
        return Result<std::uint64_t>::Success(sum);
    }

    /** @copydoc StreamingFeatureBudgetReservations::Runtime */
    const WorldStreamingRuntimeComposition &StreamingFeatureBudgetReservations::Runtime() const noexcept {
        return runtime_;
    }

    /** @copydoc StreamingFeatureBudgetReservations::SharedAssets */
    const SharedAssetResidencyLedger &StreamingFeatureBudgetReservations::SharedAssets() const noexcept {
        return shared_;
    }
}  // namespace Horo::WorldStreaming
