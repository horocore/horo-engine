#pragma once
#include "Horo/WorldStreaming/StreamingFeatureBudgetReservations.h"
#include "StreamingCellDirectionTestSupport.h"

#include <chrono>
#include <limits>

namespace Horo::WorldStreaming::FinalizationTestSupport {
    using TestSupport::IdentityFrom;
    using Feature = StreamingBudgetFeature;
    using Axis = StreamingBudgetDimension;

    [[nodiscard]] inline StreamingBudgetAmounts Amounts(const std::uint64_t cpu = 0, const std::uint64_t gpu = 0,
                                                        const std::uint64_t staging = 0, const std::uint64_t io = 0,
                                                        const std::uint64_t scratch = 0, const std::uint64_t retired = 0,
                                                        const std::uint64_t work = 0) {
        const std::array values{cpu, gpu, staging, io, scratch, retired, work};
        std::array<StreamingBudgetAmount, StreamingBudgetDimensionCount> amounts{};
        for (std::size_t i = 0; i < values.size(); ++i)
            amounts[i] = {static_cast<Axis>(i), values[i]};
        return StreamingBudgetAmounts::Create(amounts).Value();
    }

    [[nodiscard]] inline StreamingBudgetPolicy Aggregate(const std::uint64_t cap = 100) {
        std::array<StreamingBudgetLimit, StreamingBudgetDimensionCount> limits{};
        for (std::size_t i = 0; i < limits.size(); ++i)
            limits[i] = {static_cast<Axis>(i), cap, cap};
        return StreamingBudgetPolicy::Create(IdentityFrom<StreamingBudgetPolicyRevision>(3), limits, std::chrono::milliseconds{2}).Value();
    }

    [[nodiscard]] inline auto Slices(const std::uint64_t each = 20) {
        const auto amount = Amounts(each, each, each, each, each, each, each);
        return std::array{StreamingFeatureBudgetAmount{Feature::Terrain, amount}, StreamingFeatureBudgetAmount{Feature::Foliage, amount},
                          StreamingFeatureBudgetAmount{Feature::Navigation, amount},
                          StreamingFeatureBudgetAmount{Feature::Physics, amount}};
    }

    [[nodiscard]] inline auto PlanEntries(const Feature feature = Feature::Terrain, const StreamingBudgetAmounts &amount = Amounts()) {
        const auto zero = Amounts();
        std::array entries{StreamingFeatureBudgetAmount{Feature::Terrain, zero}, StreamingFeatureBudgetAmount{Feature::Foliage, zero},
                           StreamingFeatureBudgetAmount{Feature::Navigation, zero}, StreamingFeatureBudgetAmount{Feature::Physics, zero},
                           StreamingFeatureBudgetAmount{Feature::General, zero}};
        entries[static_cast<std::size_t>(feature)].amounts = amount;
        return entries;
    }

    [[nodiscard]] inline StreamingFeatureBudgetPlan Plan(const Feature feature = Feature::Terrain,
                                                         const StreamingBudgetAmounts &amount = Amounts()) {
        return StreamingFeatureBudgetPlan::Create(PlanEntries(feature, amount)).Value();
    }

    struct Host final {
        int service{};

        [[nodiscard]] inline auto Bindings() const {
            return std::array{StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(1),
                                                             IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                             StreamingRuntimeServiceRole::Planner, &service},
                              StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(2),
                                                             IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                             StreamingRuntimeServiceRole::AssetProvider, &service},
                              StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(3),
                                                             IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                             StreamingRuntimeServiceRole::SceneRuntime, &service},
                              StreamingRuntimeServiceBinding{IdentityFrom<StreamingRuntimeServiceId>(4),
                                                             IdentityFrom<StreamingRuntimeServiceRevision>(1),
                                                             StreamingRuntimeServiceRole::FeatureAdapter, &service}};
        }

        [[nodiscard]] inline StreamingFeatureBudgetConfig Config(const std::uint32_t operations = 4, const std::uint32_t entries = 4,
                                                                 const std::uint32_t leases = 8) const {
            return {1,
                    {TestSupport::WorldOwner(),
                     IdentityFrom<StreamingRuntimeCompositionRevision>(1),
                     IdentityFrom<StreamingSchedulerLedgerId>(9),
                     {operations,
                      10,
                      {.profile = WorldPartitionProjectProfile::Standalone,
                       .revision = IdentityFrom<StreamingConcurrencyRevision>(1),
                       .loads = operations,
                       .activations = operations,
                       .retirements = operations}},
                     4},
                    entries,
                    leases};
        }

        [[nodiscard]] inline StreamingFeatureBudgetReservations Ledger(const std::uint32_t operations = 4, const std::uint32_t entries = 4,
                                                                       const std::uint32_t leases = 8) const {
            auto result = StreamingFeatureBudgetReservations::Create(Config(operations, entries, leases), Bindings(),
                                                                     StreamingFeatureBudgetPolicy::Create(Aggregate(), Slices()).Value());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    };

}  // namespace Horo::WorldStreaming::FinalizationTestSupport
