#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "RenderResourceRegistry.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <new>
#include <utility>

namespace Horo::Render::RegistryRollbackTest {
    using namespace Detail;

    constexpr RenderMemoryCostPlan Cost{.memoryClass = RenderMemoryClass::PersistentDevice,
                                        .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                        .provenance = RenderMemoryCostProvenance::Estimated,
                                        .compatibility = RenderMemoryCompatibilityId{1},
                                        .payloadBytes = 64,
                                        .requiredBytes = 64,
                                        .alignment = 1};

    /** @brief Records the real budget retirement callback without allocating observation metadata. */
    struct RetirementAudit {
        std::size_t released{};
        bool succeeded{};

        void Retire(RenderMemoryBudget &budget, const std::uint64_t instance, const RenderMemoryAllocationId allocation) noexcept {
            ++released;
            const auto retiring = budget.BeginRetire(allocation);
            const auto retired = budget.AcknowledgeRetirement(allocation);
            succeeded = instance == 71 && retiring.HasValue() && retired.HasValue();
        }
    };

    /** @brief Publishes one real budget allocation to exercise ready-generation retirement during shutdown. */
    [[nodiscard]] ResourceReservation PublishBudgetedBuffer(RenderResourceRegistry &registry, RenderMemoryBudget &budget) {
        const auto ready = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(ready.HasValue());
        const auto memory = budget.Reserve({7, 1}, ready.Value().operation, Cost);
        REQUIRE(memory.HasValue());
        const auto committed = budget.Commit(memory.Value());
        REQUIRE(committed.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, ready.Value().identity, 71, committed.Value().id).HasValue());
        return ready.Value();
    }

    /** @brief Verifies retired and cancelled ownership returns every charge to the sole budget. */
    void VerifyReleasedBudget(RenderMemoryBudget &budget) {
        CHECK(budget.Snapshot().allocationCount == 0);
        CHECK(budget.Snapshot().reservationCount == 0);
        static_cast<void>(budget.ReclaimEmptyBlocks(64));
        CHECK(budget.Snapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Reservation rollback owns cancellation metadata before allocation failure",
              "[unit][runtime][renderer][resource][rollback]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        auto createdBudget = RenderMemoryBudget::Create(owner.Value(), {});
        REQUIRE(createdBudget.HasValue());
        auto budget = std::move(createdBudget).Value();
        RenderResourceRegistry registry{owner.Value(), {.maximumSlots = 1, .maximumPendingRequests = 1}};
        const auto reserved = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(reserved.HasValue());
        const auto memory = budget->Reserve({7, 1}, reserved.Value().operation, Cost);
        REQUIRE(memory.HasValue());
        Tests::AllocationProbe::Measurement allocations;
        {
            Tests::AllocationProbe::ScopedFailure failure;
            Tests::AllocationProbe::ScopedMeasurement measurement;
            {
                ResourceReservationGuard rollback{registry, RenderResourceClass::Buffer, reserved.Value()};
                rollback.OwnMemory(*budget, memory.Value());
            }
            allocations = measurement.Snapshot();
        }
        CHECK(allocations.requests == 0);
        CHECK(registry.OperationResult(reserved.Value().operation).ErrorValue().code.Value() ==
              "render.frontend.resource.operation_cancelled");
        CHECK(registry.State(RenderResourceClass::Buffer, reserved.Value().identity).ErrorValue().code.Value() ==
              "render.frontend.resource.stale");
        VerifyReleasedBudget(*budget);
        REQUIRE(registry.Reserve(RenderResourceClass::Buffer).HasValue());
        registry.Shutdown(BackendResourceReleaseMode::DestroyNative);
    }

    TEST_CASE("Reservation guard rollback releases dependency pins while commit retains exact ownership",
              "[unit][runtime][renderer][resource][rollback]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        auto createdBudget = RenderMemoryBudget::Create(owner.Value(), {});
        REQUIRE(createdBudget.HasValue());
        auto budget = std::move(createdBudget).Value();
        RetirementAudit retirement;
        RenderResourceRegistry registry{owner.Value(),
                                        {.maximumSlots = 2},
                                        [&](const RenderResourceClass, const std::uint64_t instance,
                                            const std::optional<RenderMemoryAllocationId> allocation,
                                            const BackendResourceReleaseMode) noexcept {
            retirement.Retire(*budget, instance, *allocation);
        }};
        const auto parent = PublishBudgetedBuffer(registry, *budget);
        const std::array dependencies{parent.identity};
        const auto child = registry.Reserve(RenderResourceClass::Mesh, dependencies);
        REQUIRE(child.HasValue());
        {
            Tests::AllocationProbe::ScopedFailure failure;
            ResourceReservationGuard rollback{registry, RenderResourceClass::Mesh, child.Value()};
        }
        REQUIRE(registry.Release(RenderResourceClass::Buffer, parent.identity).HasValue());
        CHECK(registry.DrainRetirements() == 1);
        CHECK(retirement.released == 1);
        CHECK(retirement.succeeded);
        const auto committed = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(committed.HasValue());
        {
            ResourceReservationGuard retained{registry, RenderResourceClass::Buffer, committed.Value()};
            retained.Commit();
        }
        CHECK(registry.State(RenderResourceClass::Buffer, committed.Value().identity).Value() == RenderResourceState::Pending);
        CHECK(registry.OperationResult(committed.Value().operation).ErrorValue().code.Value() ==
              "render.frontend.resource.operation_pending");
        registry.Shutdown(BackendResourceReleaseMode::DestroyNative);
        VerifyReleasedBudget(*budget);
    }

    TEST_CASE("Allocation failure during pending cancellation leaves allocation-free shutdown ownership",
              "[unit][runtime][renderer][resource][rollback]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        auto createdBudget = RenderMemoryBudget::Create(owner.Value(), {});
        REQUIRE(createdBudget.HasValue());
        auto budget = std::move(createdBudget).Value();
        RetirementAudit retirement;
        RenderResourceRegistry registry{owner.Value(),
                                        {.maximumPendingRequests = 1},
                                        [&](const RenderResourceClass, const std::uint64_t instance,
                                            const std::optional<RenderMemoryAllocationId> allocation,
                                            const BackendResourceReleaseMode) noexcept {
            retirement.Retire(*budget, instance, *allocation);
        }};
        const auto ready = PublishBudgetedBuffer(registry, *budget);
        const auto pending = registry.Reserve(RenderResourceClass::Texture);
        REQUIRE(pending.HasValue());
        const auto pendingMemory = budget->Reserve({7, 1}, pending.Value().operation, Cost);
        REQUIRE(pendingMemory.HasValue());
        REQUIRE(budget->Cancel(pendingMemory.Value()).HasValue());
        bool cancellationThrew = false;
        {
            Tests::AllocationProbe::ScopedFailure failure;
            try {
                static_cast<void>(registry.CancelPending(RenderResourceClass::Texture, pending.Value().identity));
            } catch (const std::bad_alloc &) {
                cancellationThrew = true;
            }
        }
        REQUIRE(cancellationThrew);
        REQUIRE(registry.State(RenderResourceClass::Texture, pending.Value().identity).Value() == RenderResourceState::Pending);
        REQUIRE(registry.OperationResult(pending.Value().operation).ErrorValue().code.Value() ==
                "render.frontend.resource.operation_pending");
        REQUIRE(registry.Reserve(RenderResourceClass::Texture).ErrorValue().code.Value() == "render.frontend.resource.queue_full");
        Tests::AllocationProbe::Measurement shutdownAllocations;
        {
            Tests::AllocationProbe::ScopedFailure failure;
            Tests::AllocationProbe::ScopedMeasurement measurement;
            registry.Shutdown(BackendResourceReleaseMode::DestroyNative);
            shutdownAllocations = measurement.Snapshot();
        }
        CHECK(shutdownAllocations.requests == 0);
        CHECK(retirement.released == 1);
        CHECK(retirement.succeeded);
        CHECK(registry.OperationResult(pending.Value().operation).ErrorValue().code.Value() == "render.frontend.resource.registry_stopped");
        CHECK(registry.OperationResult(ready.operation).HasValue());
        VerifyReleasedBudget(*budget);
    }
}  // namespace Horo::Render::RegistryRollbackTest
