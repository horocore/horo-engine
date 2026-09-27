#include "Horo/WorldStreaming/SharedAssetResidency.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        [[nodiscard]] StreamingBudgetAmounts Cost(const std::uint64_t cpu, const std::uint64_t gpu = 0) {
            const std::array amounts{
                StreamingBudgetAmount{StreamingBudgetDimension::CpuResidentBytes, cpu},
                StreamingBudgetAmount{StreamingBudgetDimension::GpuResidentBytes, gpu},
                StreamingBudgetAmount{StreamingBudgetDimension::StagingBytes, 0},
                StreamingBudgetAmount{StreamingBudgetDimension::IoBytesInFlight, 0},
                StreamingBudgetAmount{StreamingBudgetDimension::QueueScratchBytes, 0},
                StreamingBudgetAmount{StreamingBudgetDimension::RetiredBytes, 0},
                StreamingBudgetAmount{StreamingBudgetDimension::OwnerWorkNanoseconds, 0},
            };
            return StreamingBudgetAmounts::Create(amounts).Value();
        }

        [[nodiscard]] std::uint64_t CpuCharge(const SharedAssetResidencyLedger &ledger) {
            return ledger.Charged(StreamingBudgetDimension::CpuResidentBytes).Value();
        }

        [[nodiscard]] SharedAssetKey Key(const std::uint8_t asset = 1, const std::uint64_t revision = 1) {
            return {.asset = TestSupport::Asset(asset), .revision = IdentityFrom<SharedAssetRevision>(revision)};
        }

        [[nodiscard]] SharedAssetConsumer Consumer(const std::uint64_t generation = 1,
                                                   const std::optional<StreamingRuntimeServiceId> provider = std::nullopt,
                                                   const std::uint64_t epoch = 1) {
            return {.fence = {.partition = TestSupport::World(),
                              .epoch = IdentityFrom<PartitionEpoch>(epoch),
                              .cell = {1, 2, 3, 0, TestSupport::Layer()},
                              .generation = IdentityFrom<StreamingGeneration>(generation)},
                    .provider = provider};
        }

        [[nodiscard]] SharedAssetResidencyLedger Ledger(const std::uint64_t owner = 5, const std::uint64_t epoch = 1,
                                                        const SharedAssetResidencyLimits limits = {2, 3, Cost(100, 100)}) {
            auto result = SharedAssetResidencyLedger::Create(TestSupport::WorldOwner(owner, epoch), limits);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] Result<SharedAssetLease> Acquire(SharedAssetResidencyLedger &ledger, const SharedAssetKey &key,
                                                       const std::uint64_t bytes, const SharedAssetConsumer &consumer) {
            return ledger.Acquire(key, Cost(bytes), consumer, consumer.fence);
        }

        TEST_CASE("Shared asset revisions are charged once across cell and provider leases", "[unit][world_streaming][shared_asset]") {
            auto ledger = Ledger();
            const auto cell = Acquire(ledger, Key(), 60, Consumer()).Value();
            const auto provider = Acquire(ledger, Key(), 60, Consumer(1, IdentityFrom<StreamingRuntimeServiceId>(7))).Value();
            REQUIRE(ledger.ChargedCount() == 1);
            REQUIRE(CpuCharge(ledger) == 60);
            REQUIRE(ledger.LeaseCount() == 2);
            REQUIRE(ledger.Release(cell).HasValue());
            RequireError(ledger.BeginRetire(Key(), cell.charge), WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            REQUIRE(ledger.Release(provider).HasValue());
            REQUIRE(CpuCharge(ledger) == 60);
            const auto retirement = ledger.BeginRetire(Key(), cell.charge).Value();
            RequireError(Acquire(ledger, Key(), 60, Consumer(2)), WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            REQUIRE(ledger.AcknowledgeRetired(retirement).HasValue());
            REQUIRE(CpuCharge(ledger) == 0);
        }

        TEST_CASE("Shared asset rejection never publishes a partial charge or duplicate lease", "[unit][world_streaming][shared_asset]") {
            auto ledger = Ledger();
            RequireError(Acquire(ledger, {}, 60, Consumer()), WorldStreamingErrors::SharedAssetInvalid);
            RequireError(Acquire(ledger, Key(), 0, Consumer()), WorldStreamingErrors::SharedAssetInvalid);
            RequireError(Acquire(ledger, Key(), 60, Consumer(1, std::nullopt, 2)), WorldStreamingErrors::SharedAssetStale);
            RequireError(ledger.Acquire(Key(), Cost(60), Consumer(), Consumer(2).fence), WorldStreamingErrors::SharedAssetStale);
            REQUIRE(ledger.ChargedCount() == 0);
            const auto first = Acquire(ledger, Key(), 60, Consumer()).Value();
            RequireError(Acquire(ledger, Key(), 60, Consumer()), WorldStreamingErrors::SharedAssetConflict);
            RequireError(Acquire(ledger, Key(), 61, Consumer(2)), WorldStreamingErrors::SharedAssetConflict);
            RequireError(Acquire(ledger, Key(2), 50, Consumer(2)), WorldStreamingErrors::SharedAssetCapacityExceeded);
            REQUIRE(ledger.ChargedCount() == 1);
            REQUIRE(CpuCharge(ledger) == 60);
            REQUIRE(ledger.LeaseCount() == 1);
            REQUIRE(ledger.Release(first).HasValue());
            RequireError(ledger.Release(first), WorldStreamingErrors::SharedAssetStale);
        }

        TEST_CASE("Shared asset limits bound entries and leases independently", "[unit][world_streaming][shared_asset]") {
            auto ledger = Ledger(5, 1, {1, 1, Cost(100, 100)});
            const auto first = Acquire(ledger, Key(), 20, Consumer()).Value();
            RequireError(Acquire(ledger, Key(2), 20, Consumer(2)), WorldStreamingErrors::SharedAssetCapacityExceeded);
            RequireError(Acquire(ledger, Key(), 20, Consumer(2)), WorldStreamingErrors::SharedAssetCapacityExceeded);
            REQUIRE(CpuCharge(ledger) == 20);
            REQUIRE(ledger.Release(first).HasValue());
            const auto retirement = ledger.BeginRetire(Key(), first.charge).Value();
            REQUIRE(ledger.AcknowledgeRetired(retirement).HasValue());
            REQUIRE(Acquire(ledger, Key(2), 100, Consumer(2)).HasValue());
        }

        TEST_CASE("Shared asset accounting preserves independent CPU and GPU limits", "[unit][world_streaming][shared_asset]") {
            auto ledger = Ledger(5, 1, {2, 2, Cost(100, 50)});
            const auto cell = Consumer();
            REQUIRE(ledger.Acquire(Key(), Cost(20, 40), cell, cell.fence).HasValue());
            const auto next = Consumer(2);
            RequireError(ledger.Acquire(Key(2), Cost(10, 20), next, next.fence), WorldStreamingErrors::SharedAssetCapacityExceeded);
            REQUIRE(CpuCharge(ledger) == 20);
            REQUIRE(ledger.Charged(StreamingBudgetDimension::GpuResidentBytes).Value() == 40);
            RequireError(ledger.Charged(StreamingBudgetDimension::Count), WorldStreamingErrors::BudgetDimensionUnsupported);
        }

        TEST_CASE("Late cache retirement cannot clear a successor charge for the same asset revision",
                  "[unit][world_streaming][shared_asset]") {
            auto ledger = Ledger();
            const auto first = Acquire(ledger, Key(), 40, Consumer()).Value();
            REQUIRE(ledger.Release(first).HasValue());
            const auto retired = ledger.BeginRetire(Key(), first.charge).Value();
            REQUIRE(ledger.AcknowledgeRetired(retired).HasValue());
            const auto next = Acquire(ledger, Key(), 40, Consumer(2)).Value();
            RequireError(ledger.BeginRetire(Key(), first.charge), WorldStreamingErrors::SharedAssetStale);
            RequireError(ledger.AcknowledgeRetired(retired), WorldStreamingErrors::SharedAssetStale);
            REQUIRE(CpuCharge(ledger) == 40);
            REQUIRE(ledger.LeaseCount() == 1);
            REQUIRE(ledger.Release(next).HasValue());
        }

        TEST_CASE("Cancellation and partition replacement retain old charges until exact retirement",
                  "[unit][world_streaming][shared_asset]") {
            auto old = Ledger();
            const auto oldLease = Acquire(old, Key(), 60, Consumer()).Value();
            REQUIRE(old.CancelAttempt(oldLease.consumer.fence).HasValue());
            REQUIRE(old.CancelAttempt(oldLease.consumer.fence).HasValue());
            RequireError(Acquire(old, Key(2), 10, Consumer()), WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            auto replacement = Ledger(6, 2);
            const auto newLease = Acquire(replacement, Key(), 60, Consumer(1, std::nullopt, 2)).Value();
            old.BeginShutdown();
            RequireError(Acquire(old, Key(2), 10, Consumer(2)), WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            RequireError(old.Release(newLease), WorldStreamingErrors::SharedAssetStale);
            REQUIRE(old.State() == SharedAssetResidencyState::Draining);
            REQUIRE(old.Release(oldLease).HasValue());
            REQUIRE(CpuCharge(old) == 60);
            const auto retirement = old.BeginRetire(Key(), oldLease.charge).Value();
            REQUIRE(old.AcknowledgeRetired(retirement).HasValue());
            REQUIRE(old.State() == SharedAssetResidencyState::Closed);
            REQUIRE(CpuCharge(replacement) == 60);
            REQUIRE(replacement.Release(newLease).HasValue());
        }

        TEST_CASE("Moving shared asset authority closes moved-from admission", "[unit][world_streaming][shared_asset]") {
            auto source = Ledger();
            const auto lease = Acquire(source, Key(), 40, Consumer()).Value();
            auto destination = std::move(source);
            REQUIRE(source.State() == SharedAssetResidencyState::Closed);
            RequireError(Acquire(source, Key(2), 10, Consumer(2)), WorldStreamingErrors::SharedAssetLifecycleUnavailable);
            REQUIRE(destination.Release(lease).HasValue());
            const auto retirement = destination.BeginRetire(Key(), lease.charge).Value();
            REQUIRE(destination.AcknowledgeRetired(retirement).HasValue());
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
