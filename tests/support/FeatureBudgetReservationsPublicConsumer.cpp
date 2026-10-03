#include "Horo/WorldStreaming/StreamingFeatureBudgetReservations.h"

#include <array>
#include <chrono>
#include <memory>
#include <utility>

namespace {
    using namespace Horo::WorldStreaming;

    template <typename T> T Id(const std::uint64_t value) {
        return T::Create(value).Value();
    }

    StreamingBudgetAmounts Bytes(const std::uint64_t cpu) {
        std::array<StreamingBudgetAmount, StreamingBudgetDimensionCount> amounts{};
        for (std::size_t i = 0; i < amounts.size(); ++i)
            amounts[i] = {static_cast<StreamingBudgetDimension>(i), i == 0 ? cpu : 0};
        return StreamingBudgetAmounts::Create(amounts).Value();
    }

    Horo::Result<StreamingFeatureBudgetReservations> CreateHost(int &hostService, const StreamingRuntimeOwnerToken &owner) {
        std::array<StreamingRuntimeServiceBinding, 4> bindings{};
        for (std::size_t i = 0; i < bindings.size(); ++i)
            bindings[i] = {Id<StreamingRuntimeServiceId>(i + 1), Id<StreamingRuntimeServiceRevision>(1),
                           static_cast<StreamingRuntimeServiceRole>(i), &hostService};
        std::array<StreamingBudgetLimit, StreamingBudgetDimensionCount> limits{};
        for (std::size_t i = 0; i < limits.size(); ++i)
            limits[i] = {static_cast<StreamingBudgetDimension>(i), 100, 100};
        const auto aggregate =
            StreamingBudgetPolicy::Create(Id<StreamingBudgetPolicyRevision>(1), limits, std::chrono::milliseconds{2}).Value();
        const std::array slices{StreamingFeatureBudgetAmount{StreamingBudgetFeature::Terrain, Bytes(20)},
                                StreamingFeatureBudgetAmount{StreamingBudgetFeature::Foliage, Bytes(20)},
                                StreamingFeatureBudgetAmount{StreamingBudgetFeature::Navigation, Bytes(20)},
                                StreamingFeatureBudgetAmount{StreamingBudgetFeature::Physics, Bytes(20)}};
        const auto policy = StreamingFeatureBudgetPolicy::Create(aggregate, slices).Value();
        const StreamingFeatureBudgetConfig
            config{1, {owner, Id<StreamingRuntimeCompositionRevision>(1), Id<StreamingSchedulerLedgerId>(1), {2, 2}, 1}, 2, 2};
        return StreamingFeatureBudgetReservations::Create(config, bindings, policy);
    }

    bool CompleteOperation(StreamingFeatureBudgetReservations &ledger, const StreamingFeatureBudgetReservation &reservation) {
        return ledger.Advance(ledger.Context(), reservation, StreamingCellOperationTransition::BeginPreparation).HasValue() &&
               ledger.Advance(ledger.Context(), reservation, StreamingCellOperationTransition::Complete).HasValue() &&
               ledger.Release(ledger.Context(), reservation).HasValue();
    }

    bool FinishShutdown(StreamingFeatureBudgetReservations &ledger, const SharedAssetRetirement &retirement) {
        return ledger.AcknowledgeSharedRetired(ledger.Context(), retirement).HasValue() &&
               ledger.Used(StreamingBudgetDimension::CpuResidentBytes).Value() == 0 && ledger.BeginShutdown(ledger.Context()).HasValue() &&
               ledger.State() == StreamingSchedulerAdmissionState::Closed;
    }

    int ExerciseCache(StreamingFeatureBudgetReservations &ledger, const StreamingFeatureBudgetReservation &reservation,
                      const StreamingFence &fence) {
        // The cache owns bytes; the aggregate ledger receives only the charge and exact consumer identity.
        auto cachedBytes = std::make_unique<std::array<std::byte, 20>>();
        std::array<std::uint8_t, 16> assetBytes{};
        assetBytes.back() = 1;
        const SharedAssetKey key{Horo::Assets::AssetId::FromBytes(assetBytes), Id<SharedAssetRevision>(1)};
        const auto lease = ledger.RealizeShared(ledger.Context(), reservation,
                                                {StreamingBudgetFeature::Terrain, key, Bytes(20), Bytes(20), {fence, std::nullopt}});
        if (lease.HasError())
            return 3;
        if (!CompleteOperation(ledger, reservation))
            return 4;
        if (ledger.Used(StreamingBudgetDimension::CpuResidentBytes).Value() != 20 ||
            ledger.ReleaseShared(ledger.Context(), lease.Value()).HasError())
            return 5;
        const auto retirement = ledger.BeginRetireShared(ledger.Context(), key, lease.Value().charge);
        if (retirement.HasError())
            return 6;
        cachedBytes.reset();
        if (!FinishShutdown(ledger, retirement.Value()))
            return 7;
        return 0;
    }

}  // namespace

int main() {
    using namespace Horo::WorldStreaming;
    int hostService{};
    SerializedWorldPartitionId partitionBytes{};
    partitionBytes.back() = 1;
    const auto partition = WorldPartitionId::Create(partitionBytes).Value();
    const StreamingRuntimeOwnerToken owner{partition, Id<PartitionEpoch>(1), Id<StreamingRuntimeOwnerId>(1)};
    auto result = CreateHost(hostService, owner);
    if (result.HasError())
        return 1;
    auto ledger = std::move(result).Value();
    const std::array peak{StreamingFeatureBudgetAmount{StreamingBudgetFeature::Terrain, Bytes(20)},
                          StreamingFeatureBudgetAmount{StreamingBudgetFeature::Foliage, Bytes(0)},
                          StreamingFeatureBudgetAmount{StreamingBudgetFeature::Navigation, Bytes(0)},
                          StreamingFeatureBudgetAmount{StreamingBudgetFeature::Physics, Bytes(0)},
                          StreamingFeatureBudgetAmount{StreamingBudgetFeature::General, Bytes(0)}};
    const StreamingFence fence{partition, owner.epoch, {0, 0, 0, 0, Id<StreamingLayerId>(1)}, Id<StreamingGeneration>(1)};
    const auto operation =
        StreamingCellOperation::Create({Id<StreamingCellOperationId>(1), fence}, StreamingCellOperationKind::Load).Value();
    const auto admitted = ledger.TryAdmit(ledger.Context(), operation, StreamingFeatureBudgetPlan::Create(peak).Value(), 1, fence);
    if (admitted.HasError())
        return 2;
    return ExerciseCache(ledger, admitted.Value(), fence);
}
