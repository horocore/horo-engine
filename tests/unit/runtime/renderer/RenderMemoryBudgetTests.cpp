#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "Horo/Runtime/Render/RenderMemoryBudgetErrors.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <limits>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    constexpr RenderResourceOwnerId Renderer{7};
    constexpr RenderResourceOwnerId OtherRenderer{8};
    constexpr RenderMemoryScopeId FirstScope{11, 1};

    [[nodiscard]] RenderMemoryBudgetConfig Config() {
        return {.hardCapBytes = 128,
                .defaultBlockBytes = 64,
                .maximumBlockBytes = 64,
                .maximumAlignment = 32,
                .maximumPools = 4,
                .maximumBlocks = 4,
                .maximumReservations = 4,
                .maximumAllocations = 8,
                .revision = 3};
    }

    [[nodiscard]] RenderMemoryCostPlan Suballocated(const std::size_t payload, const std::size_t required,
                                                    const std::size_t alignment = 1) {
        return {.memoryClass = RenderMemoryClass::PersistentDevice,
                .allocationClass = RenderMemoryAllocationClass::Suballocated,
                .provenance = RenderMemoryCostProvenance::Exact,
                .compatibility = RenderMemoryCompatibilityId{1},
                .payloadBytes = payload,
                .requiredBytes = required,
                .alignment = alignment};
    }

    [[nodiscard]] RenderMemoryCostPlan Dedicated(const std::size_t bytes) {
        return {.memoryClass = RenderMemoryClass::PersistentDevice,
                .allocationClass = RenderMemoryAllocationClass::Dedicated,
                .provenance = RenderMemoryCostProvenance::Estimated,
                .compatibility = RenderMemoryCompatibilityId{1},
                .payloadBytes = bytes,
                .requiredBytes = bytes,
                .alignment = 1};
    }

    [[nodiscard]] std::unique_ptr<RenderMemoryBudget> CreateBudget(const RenderMemoryBudgetConfig config = Config()) {
        auto created = RenderMemoryBudget::Create(Renderer, config);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    TEST_CASE("Render memory configuration rejects invalid and inconsistent bounds", "[unit][runtime][renderer][memory]") {
        REQUIRE(Config().IsValid());
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.hardCapBytes = 0}.IsValid());
        REQUIRE_FALSE((RenderMemoryBudgetConfig{.hardCapBytes = 64, .defaultBlockBytes = 65, .maximumBlockBytes = 64}.IsValid()));
        REQUIRE_FALSE((RenderMemoryBudgetConfig{.hardCapBytes = 64, .defaultBlockBytes = 32, .maximumBlockBytes = 65}.IsValid()));
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.maximumAlignment = 3}.IsValid());
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.maximumPools = 0}.IsValid());
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.maximumBlocks = 0}.IsValid());
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.maximumReservations = 0}.IsValid());
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.maximumAllocations = 0}.IsValid());
        REQUIRE_FALSE((RenderMemoryBudgetConfig{.maximumReservations = 2, .maximumAllocations = 1}.IsValid()));
        REQUIRE_FALSE(RenderMemoryBudgetConfig{.revision = 0}.IsValid());

        const auto invalidOwner = RenderMemoryBudget::Create({}, Config());
        REQUIRE(invalidOwner.HasError());
        REQUIRE(invalidOwner.ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidConfiguration.code.Value());

        const auto allocationFailure = [] {
            Tests::AllocationProbe::ScopedFailure failure;
            return RenderMemoryBudget::Create(Renderer, Config());
        }();
        REQUIRE(allocationFailure.HasError());
        CHECK(allocationFailure.ErrorValue().code.Value() == RenderMemoryBudgetErrors::CapacityExceeded.code.Value());
    }

    TEST_CASE("Render memory ledger aligns suballocations and charges each backing block once", "[unit][runtime][renderer][memory]") {
        auto budget = CreateBudget();

        auto firstReservation = budget->Reserve(FirstScope, ResourceOperationId{21}, Suballocated(20, 24, 16));
        REQUIRE(firstReservation.HasValue());
        const auto placement = budget->Placement(firstReservation.Value());
        REQUIRE(placement.HasValue());
        CHECK(placement.Value().IsValid());
        CHECK(placement.Value().scope == FirstScope);
        CHECK(placement.Value().attempt == ResourceOperationId{21});
        CHECK(placement.Value().offsetBytes == 0);
        CHECK(placement.Value().backingBytes == 64);
        auto secondReservation = budget->Reserve(FirstScope, ResourceOperationId{22}, Suballocated(15, 16, 16));
        REQUIRE(secondReservation.HasValue());
        const auto secondPlacement = budget->Placement(secondReservation.Value());
        REQUIRE(secondPlacement.HasValue());
        CHECK(secondPlacement.Value().pool == placement.Value().pool);
        CHECK(secondPlacement.Value().offsetBytes == 32);
        const auto reserved = budget->Snapshot();
        CHECK(reserved.reservedUnallocatedBytes == 64);
        CHECK(reserved.reservedPayloadBytes == 35);
        CHECK(reserved.committedBackingBytes == 0);
        CHECK(reserved.blockCount == 1);

        auto first = budget->Commit(firstReservation.Value());
        REQUIRE(first.HasValue());
        CHECK(budget->Placement(firstReservation.Value()).ErrorValue().code.Value() ==
              RenderMemoryBudgetErrors::InvalidReservation.code.Value());
        CHECK(first.Value().attempt == ResourceOperationId{21});
        CHECK(first.Value().budgetRevision == 3);
        CHECK(first.Value().offsetBytes == 0);
        CHECK(first.Value().backingBytes == 64);
        CHECK(first.Value().allocationClass == RenderMemoryAllocationClass::Suballocated);

        auto second = budget->Commit(secondReservation.Value());
        REQUIRE(second.HasValue());
        CHECK(second.Value().pool == first.Value().pool);
        CHECK(second.Value().offsetBytes == 32);

        const auto snapshot = budget->Snapshot();
        CHECK(snapshot.committedBackingBytes == 64);
        CHECK(snapshot.livePayloadBytes == 35);
        CHECK(snapshot.reusableSlackBytes == 24);
        CHECK(snapshot.peakChargedBytes == 64);
        CHECK(snapshot.reservationCount == 0);
        CHECK(snapshot.allocationCount == 2);
        CHECK(snapshot.externalFragmentationBasisPoints == 3333);

        const auto pool = budget->PoolSnapshot(first.Value().pool);
        REQUIRE(pool.HasValue());
        CHECK(pool.Value().scope == FirstScope);
        CHECK(pool.Value().compatibility == RenderMemoryCompatibilityId{1});
        CHECK(pool.Value().committedBackingBytes == 64);
        CHECK(pool.Value().livePayloadBytes == 35);
        CHECK(pool.Value().reusableSlackBytes == 24);
        CHECK(pool.Value().externalFragmentationBasisPoints == 3333);
    }

    TEST_CASE("Render memory ledger separates compatible pools and excludes dedicated backing from reusable slack",
              "[unit][runtime][renderer][memory]") {
        auto budget = CreateBudget();
        auto pooledReservation = budget->Reserve(FirstScope, ResourceOperationId{31}, Suballocated(16, 16));
        RenderMemoryCostPlan dedicatedPlan = Dedicated(32);
        dedicatedPlan.compatibility = RenderMemoryCompatibilityId{2};
        auto dedicatedReservation = budget->Reserve(FirstScope, ResourceOperationId{32}, dedicatedPlan);
        REQUIRE(pooledReservation.HasValue());
        REQUIRE(dedicatedReservation.HasValue());
        auto pooled = budget->Commit(pooledReservation.Value());
        auto dedicated = budget->Commit(dedicatedReservation.Value());
        REQUIRE(pooled.HasValue());
        REQUIRE(dedicated.HasValue());

        CHECK(pooled.Value().pool != dedicated.Value().pool);
        CHECK(dedicated.Value().allocationClass == RenderMemoryAllocationClass::Dedicated);
        const auto snapshot = budget->Snapshot();
        CHECK(snapshot.committedBackingBytes == 96);
        CHECK(snapshot.reusableSlackBytes == 48);
        CHECK(snapshot.poolCount == 2);

        const auto dedicatedPool = budget->PoolSnapshot(dedicated.Value().pool);
        REQUIRE(dedicatedPool.HasValue());
        CHECK(dedicatedPool.Value().reusableSlackBytes == 0);
        CHECK(dedicatedPool.Value().externalFragmentationBasisPoints == 0);

        const auto foreignPool = budget->PoolSnapshot(RenderMemoryPoolId{OtherRenderer, dedicated.Value().pool.value});
        REQUIRE(foreignPool.HasError());
        CHECK(foreignPool.ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidPool.code.Value());
    }

    TEST_CASE("Render memory reservation failures are typed and leave accounting coherent", "[unit][runtime][renderer][memory]") {
        auto budget = CreateBudget();

        const auto invalidRequest = budget->Reserve({}, ResourceOperationId{1}, Suballocated(1, 1));
        REQUIRE(invalidRequest.HasError());
        CHECK(invalidRequest.ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidRequest.code.Value());

        const auto invalidPlan = budget->Reserve(FirstScope, ResourceOperationId{1}, Suballocated(0, 1));
        REQUIRE(invalidPlan.HasError());
        CHECK(invalidPlan.ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidCostPlan.code.Value());

        RenderMemoryCostPlan incompatible = Suballocated(1, 1);
        incompatible.compatibility = {};
        const auto missingCompatibility = budget->Reserve(FirstScope, ResourceOperationId{1}, incompatible);
        REQUIRE(missingCompatibility.HasError());
        CHECK(missingCompatibility.ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidCostPlan.code.Value());

        const auto unsupported = budget->Reserve(FirstScope, ResourceOperationId{1}, Suballocated(65, 65));
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == RenderMemoryBudgetErrors::UnsupportedAllocation.code.Value());

        auto firstReservation = budget->Reserve(FirstScope, ResourceOperationId{2}, Dedicated(96));
        REQUIRE(firstReservation.HasValue());
        REQUIRE(budget->Commit(firstReservation.Value()).HasValue());
        const auto denied = budget->Reserve(FirstScope, ResourceOperationId{3}, Dedicated(33));
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderMemoryBudgetErrors::BudgetExceeded.code.Value());
        const auto snapshot = budget->Snapshot();
        CHECK(snapshot.committedBackingBytes == 96);
        CHECK(snapshot.reservedUnallocatedBytes == 0);
        CHECK(snapshot.failedReservationCount == 1);
    }

    TEST_CASE("Render memory cancellation and retirement release charges only after bounded reclaim", "[unit][runtime][renderer][memory]") {
        auto budget = CreateBudget();
        auto cancelled = budget->Reserve(FirstScope, ResourceOperationId{41}, Suballocated(8, 8));
        REQUIRE(cancelled.HasValue());
        REQUIRE(budget->Cancel(cancelled.Value()).HasValue());
        CHECK(budget->Snapshot().blockCount == 0);
        CHECK(budget->Cancel(cancelled.Value()).ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidReservation.code.Value());

        auto reservation = budget->Reserve(FirstScope, ResourceOperationId{42}, Suballocated(8, 8));
        REQUIRE(reservation.HasValue());
        auto allocation = budget->Commit(reservation.Value());
        REQUIRE(allocation.HasValue());
        CHECK(budget->Cancel(reservation.Value()).ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidReservation.code.Value());
        CHECK(budget->AcknowledgeRetirement(allocation.Value().id).ErrorValue().code.Value() ==
              RenderMemoryBudgetErrors::InvalidAllocation.code.Value());
        CHECK(budget->BeginRetire(RenderMemoryAllocationId{OtherRenderer, allocation.Value().id.value}).ErrorValue().code.Value() ==
              RenderMemoryBudgetErrors::InvalidAllocation.code.Value());
        auto slackReservation = budget->Reserve(FirstScope, ResourceOperationId{43}, Suballocated(8, 8));
        REQUIRE(slackReservation.HasValue());
        REQUIRE(budget->Cancel(slackReservation.Value()).HasValue());
        CHECK(budget->Snapshot().committedBackingBytes == 64);
        REQUIRE(budget->BeginRetire(allocation.Value().id).HasValue());
        CHECK(budget->Snapshot().retiringPayloadBytes == 8);
        CHECK(budget->ReclaimEmptyBlocks(1) == 0);
        REQUIRE(budget->AcknowledgeRetirement(allocation.Value().id).HasValue());
        CHECK(budget->Snapshot().committedBackingBytes == 64);
        CHECK(budget->ReclaimEmptyBlocks(0) == 0);
        CHECK(budget->ReclaimEmptyBlocks(1) == 64);
        CHECK(budget->Snapshot().committedBackingBytes == 0);
        CHECK(budget->BeginRetire(allocation.Value().id).ErrorValue().code.Value() ==
              RenderMemoryBudgetErrors::InvalidAllocation.code.Value());
    }

    TEST_CASE("Render memory hard-cap revisions preserve prior claims and expose over-budget state", "[unit][runtime][renderer][memory]") {
        auto budget = CreateBudget();
        auto reservation = budget->Reserve(FirstScope, ResourceOperationId{51}, Suballocated(8, 8));
        REQUIRE(reservation.HasValue());
        auto allocation = budget->Commit(reservation.Value());
        REQUIRE(allocation.HasValue());

        REQUIRE(budget->ReviseHardCap(32, 4).HasValue());
        CHECK(budget->Snapshot().overBudget);
        CHECK(allocation.Value().budgetRevision == 3);
        const auto denied = budget->Reserve(FirstScope, ResourceOperationId{52}, Suballocated(8, 8));
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderMemoryBudgetErrors::BudgetExceeded.code.Value());
        CHECK(budget->ReviseHardCap(128, 4).ErrorValue().code.Value() == RenderMemoryBudgetErrors::InvalidConfiguration.code.Value());
        REQUIRE(budget->ReviseHardCap(128, 5).HasValue());
        CHECK_FALSE(budget->Snapshot().overBudget);
    }

    TEST_CASE("Render memory metadata bounds and shutdown are deterministic", "[unit][runtime][renderer][memory]") {
        RenderMemoryBudgetConfig config = Config();
        config.maximumPools = 1;
        config.maximumBlocks = 2;
        config.maximumReservations = 1;
        config.maximumAllocations = 1;
        auto budget = CreateBudget(config);

        auto reservation = budget->Reserve(FirstScope, ResourceOperationId{61}, Suballocated(8, 8));
        REQUIRE(reservation.HasValue());
        const auto full = budget->Reserve(FirstScope, ResourceOperationId{62}, Suballocated(8, 8));
        REQUIRE(full.HasError());
        CHECK(full.ErrorValue().code.Value() == RenderMemoryBudgetErrors::CapacityExceeded.code.Value());
        REQUIRE(budget->Commit(reservation.Value()).HasValue());

        budget->Shutdown();
        budget->Shutdown();
        const auto stopped = budget->Reserve(FirstScope, ResourceOperationId{63}, Suballocated(8, 8));
        REQUIRE(stopped.HasError());
        CHECK(stopped.ErrorValue().code.Value() == RenderMemoryBudgetErrors::Stopped.code.Value());
        const auto snapshot = budget->Snapshot();
        CHECK_FALSE(snapshot.acceptingReservations);
        CHECK(snapshot.poolCount == 0);
        CHECK(snapshot.blockCount == 0);
        CHECK(snapshot.reservationCount == 0);
        CHECK(snapshot.allocationCount == 0);
    }

    TEST_CASE("Render memory ledger enforces pool and block table bounds", "[unit][runtime][renderer][memory]") {
        RenderMemoryBudgetConfig poolConfig = Config();
        poolConfig.maximumPools = 1;
        auto poolBudget = CreateBudget(poolConfig);
        auto firstPoolReservation = poolBudget->Reserve(FirstScope, ResourceOperationId{64}, Suballocated(8, 8));
        REQUIRE(firstPoolReservation.HasValue());
        REQUIRE(poolBudget->Commit(firstPoolReservation.Value()).HasValue());
        const auto poolFull = poolBudget->Reserve(RenderMemoryScopeId{12, 1}, ResourceOperationId{65}, Dedicated(8));
        REQUIRE(poolFull.HasError());
        CHECK(poolFull.ErrorValue().code.Value() == RenderMemoryBudgetErrors::CapacityExceeded.code.Value());

        RenderMemoryBudgetConfig blockConfig = Config();
        blockConfig.maximumBlocks = 1;
        auto blockBudget = CreateBudget(blockConfig);
        auto firstBlockReservation = blockBudget->Reserve(FirstScope, ResourceOperationId{66}, Dedicated(8));
        REQUIRE(firstBlockReservation.HasValue());
        REQUIRE(blockBudget->Commit(firstBlockReservation.Value()).HasValue());
        const auto blockFull = blockBudget->Reserve(FirstScope, ResourceOperationId{67}, Dedicated(8));
        REQUIRE(blockFull.HasError());
        CHECK(blockFull.ErrorValue().code.Value() == RenderMemoryBudgetErrors::CapacityExceeded.code.Value());
    }

    TEST_CASE("Render memory steady-state success operations use preallocated metadata", "[unit][runtime][renderer][memory]") {
        auto budget = CreateBudget();
        auto firstReservation = budget->Reserve(FirstScope, ResourceOperationId{71}, Suballocated(8, 8));
        REQUIRE(firstReservation.HasValue());
        auto first = budget->Commit(firstReservation.Value());
        REQUIRE(first.HasValue());

        const std::size_t before = Tests::AllocationProbe::Count();
        auto reservation = budget->Reserve(FirstScope, ResourceOperationId{72}, Suballocated(8, 8));
        auto allocation = budget->Commit(reservation.Value());
        const auto snapshot = budget->Snapshot();
        const auto poolSnapshot = budget->PoolSnapshot(allocation.Value().pool);
        const auto retiring = budget->BeginRetire(allocation.Value().id);
        const auto retired = budget->AcknowledgeRetirement(allocation.Value().id);
        const std::size_t after = Tests::AllocationProbe::Count();

        REQUIRE(reservation.HasValue());
        REQUIRE(allocation.HasValue());
        REQUIRE(poolSnapshot.HasValue());
        REQUIRE(retiring.HasValue());
        REQUIRE(retired.HasValue());
        CHECK(snapshot.allocationCount == 2);
        CHECK(after == before);
    }

    TEST_CASE("Render memory cost validation checks overflow-prone size and alignment boundaries", "[unit][runtime][renderer][memory]") {
        CHECK(Suballocated(1, 1, 1).IsValid());
        CHECK_FALSE(Suballocated(2, 1, 1).IsValid());
        CHECK_FALSE(Suballocated(1, 1, 0).IsValid());
        CHECK_FALSE(Suballocated(1, 1, 3).IsValid());

        RenderMemoryBudgetConfig config = Config();
        config.hardCapBytes = std::numeric_limits<std::size_t>::max();
        config.maximumBlockBytes = std::numeric_limits<std::size_t>::max();
        config.defaultBlockBytes = std::numeric_limits<std::size_t>::max() - 1;
        auto budget = CreateBudget(config);
        const auto overflow = budget->Reserve(FirstScope, ResourceOperationId{81}, Suballocated(1, 1, 32));
        REQUIRE(overflow.HasError());
        CHECK(overflow.ErrorValue().code.Value() == RenderMemoryBudgetErrors::UnsupportedAllocation.code.Value());
    }
}  // namespace
