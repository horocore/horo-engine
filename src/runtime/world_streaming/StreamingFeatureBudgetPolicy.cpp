#include "Horo/WorldStreaming/StreamingFeatureBudgetReservations.h"
#include "WorldStreamingInternal.h"

namespace Horo::WorldStreaming {
    namespace {
        using Internal::Failure;
        using Axes = std::array<std::uint64_t, StreamingBudgetDimensionCount>;

        /** @brief Builds an already-complete canonical vector without inferring missing axes. */
        [[nodiscard]] StreamingBudgetAmounts AmountsFrom(const Axes &values) {
            std::array<StreamingBudgetAmount, StreamingBudgetDimensionCount> amounts{};
            for (std::size_t index = 0; index < values.size(); ++index)
                amounts[index] = {static_cast<StreamingBudgetDimension>(index), values[index]};
            return StreamingBudgetAmounts::Create(amounts).Value();
        }
    }  // namespace

    /** @copydoc StreamingFeatureBudgetPlan::Create */
    Result<StreamingFeatureBudgetPlan> StreamingFeatureBudgetPlan::Create(const std::span<const StreamingFeatureBudgetAmount> amounts) {
        if (amounts.size() != StreamingBudgetFeatureCount)
            return Failure<StreamingFeatureBudgetPlan>(WorldStreamingErrors::FeatureBudgetInvalid);
        Matrix values{};
        std::array<bool, StreamingBudgetFeatureCount> present{};
        for (const auto &amount : amounts) {
            const auto index = static_cast<std::size_t>(amount.feature);
            if (index >= present.size())
                return Failure<StreamingFeatureBudgetPlan>(WorldStreamingErrors::FeatureBudgetUnsupported);
            if (present[index])
                return Failure<StreamingFeatureBudgetPlan>(WorldStreamingErrors::FeatureBudgetInvalid);
            present[index] = true;
            for (const auto &axis : amount.amounts.Entries())
                values[index][static_cast<std::size_t>(axis.dimension)] = axis.value;
        }
        return Result<StreamingFeatureBudgetPlan>::Success(StreamingFeatureBudgetPlan{values});
    }

    /** @copydoc StreamingFeatureBudgetPlan::Amounts */
    Result<StreamingBudgetAmounts> StreamingFeatureBudgetPlan::Amounts(const StreamingBudgetFeature feature) const {
        const auto index = static_cast<std::size_t>(feature);
        if (index >= values_.size())
            return Failure<StreamingBudgetAmounts>(WorldStreamingErrors::FeatureBudgetUnsupported);
        return Result<StreamingBudgetAmounts>::Success(AmountsFrom(values_[index]));
    }

    /** @copydoc StreamingFeatureBudgetPolicy::Create */
    Result<StreamingFeatureBudgetPolicy> StreamingFeatureBudgetPolicy::Create(const StreamingBudgetPolicy &aggregate,
                                                                              const std::span<const StreamingFeatureBudgetAmount> slices) {
        if (slices.size() != StreamingBudgetFeatureCount - 1)
            return Failure<StreamingFeatureBudgetPolicy>(WorldStreamingErrors::FeatureBudgetInvalid);
        Axes global{};
        for (const auto &limit : aggregate.Limits())
            global[static_cast<std::size_t>(limit.dimension)] = limit.hardLimit;
        auto remaining = global;
        StreamingFeatureBudgetPlan::Matrix values{};
        std::array<bool, StreamingBudgetFeatureCount - 1> present{};
        for (const auto &slice : slices) {
            const auto index = static_cast<std::size_t>(slice.feature);
            if (index >= present.size())
                return Failure<StreamingFeatureBudgetPolicy>(WorldStreamingErrors::FeatureBudgetUnsupported);
            if (present[index])
                return Failure<StreamingFeatureBudgetPolicy>(WorldStreamingErrors::FeatureBudgetInvalid);
            present[index] = true;
            for (const auto &amount : slice.amounts.Entries()) {
                const auto axis = static_cast<std::size_t>(amount.dimension);
                if (amount.value > remaining[axis])
                    return Failure<StreamingFeatureBudgetPolicy>(WorldStreamingErrors::FeatureBudgetCapacityExceeded);
                remaining[axis] -= amount.value;
                values[index][axis] = amount.value;
            }
        }
        values[static_cast<std::size_t>(StreamingBudgetFeature::General)] = remaining;
        return Result<StreamingFeatureBudgetPolicy>::Success(
            StreamingFeatureBudgetPolicy{aggregate.Revision(), global, StreamingFeatureBudgetPlan{values}});
    }

    /** @copydoc StreamingFeatureBudgetPolicy::GlobalCapacity */
    StreamingBudgetAmounts StreamingFeatureBudgetPolicy::GlobalCapacity() const {
        return AmountsFrom(global_);
    }

    /** @copydoc StreamingFeatureBudgetPolicy::Capacity */
    Result<StreamingBudgetAmounts> StreamingFeatureBudgetPolicy::Capacity(const StreamingBudgetFeature feature) const {
        return capacity_.Amounts(feature);
    }

    /** @copydoc StreamingFeatureBudgetPolicy::Revision */
    StreamingBudgetPolicyRevision StreamingFeatureBudgetPolicy::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc StreamingFeatureBudgetReservation::IsValid */
    bool StreamingFeatureBudgetReservation::IsValid() const noexcept {
        return owner.IsValid() && scheduler.IsValid();
    }
}  // namespace Horo::WorldStreaming
