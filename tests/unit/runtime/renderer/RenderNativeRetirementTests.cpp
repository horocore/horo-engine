#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "RenderMemoryTestSupport.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace Horo::Render::NativeRetirementTest {
    using namespace Detail;

    /** @brief Models actual asynchronous backing ownership while exercising the sole real frontend budget/registry. */
    struct Fixture {
        std::unique_ptr<RenderMemoryBudget> budget;
        std::unique_ptr<RenderResourceRegistry> registry;
        std::unique_ptr<IRenderBackend> backend;
        std::optional<RenderResourceNativeLifetime> nativeBacking;
        RenderResourceUploadQueue::Request request;
        int destroyRequests{};

        Fixture() {
            const auto owner = AcquireRenderResourceOwnerId();
            REQUIRE(owner.HasValue());
            auto created = RenderMemoryBudget::Create(owner.Value(), {});
            REQUIRE(created.HasValue());
            budget = std::move(created).Value();
            registry =
                std::make_unique<RenderResourceRegistry>(owner.Value(), RenderResourceRegistryLimits{.maximumSlots = 4},
                                                         [this](RenderResourceClass, std::uint64_t, std::optional<RenderMemoryAllocationId>,
                                                                BackendResourceReleaseMode) {
                ++destroyRequests;
                // Destroy requests do not prove the accepted copy or its backing terminal.
            },
                                                         budget.get());
            RenderBackendRegistry backends;
            REQUIRE(RegisterNullRenderBackend(backends).HasValue());
            REQUIRE(backends.Seal().HasValue());
            auto selected = backends.Create(RenderBackendId{"null"});
            REQUIRE(selected.HasValue());
            backend = std::move(selected).Value();
            const auto resource = registry->Reserve(RenderResourceClass::Buffer);
            REQUIRE(resource.HasValue());
            const auto memory = budget->Reserve({7, 1}, resource.Value().operation, TestSupport::DedicatedMemoryCost(64, 1));
            REQUIRE(memory.HasValue());
            request.kind = RenderResourceUploadQueue::RequestKind::Buffer;
            request.identity = resource.Value().identity;
            request.memoryReservation = memory.Value();
            auto prepared = registry->PrepareNativeLifetime(RenderResourceClass::Buffer, request.identity, memory.Value(), budget.get());
            REQUIRE(prepared.HasValue());
            nativeBacking.emplace(std::move(prepared).Value());
        }

        ~Fixture() {
            // Assertion unwinding still keeps the simulated native owner ahead of its borrowed registry.
            nativeBacking.reset();
            registry->Shutdown(BackendResourceReleaseMode::NativeAlreadyReleased);
        }

        void Complete(const Result<std::uint64_t> &created) {
            CompleteResourceRequest(*backend, *budget, *registry, request, created);
        }

        [[nodiscard]] static Error NativeFailure() {
            return {ErrorCode{"render.test.accepted_copy_failed"},
                    ErrorDomainId{"horo.render.test"},
                    ErrorSeverity::Error,
                    "Native creation failed after accepting a copy; backing is still owned.",
                    {}};
        }
    };

    TEST_CASE("Accepted failed native creation retains its original reservation without an instance until real release",
              "[unit][renderer][resource][retirement]") {
        Fixture fixture;
        const auto charged = fixture.budget->Snapshot();
        REQUIRE(charged.reservationCount == 1);
        fixture.Complete(Result<std::uint64_t>::Failure(Fixture::NativeFailure()));
        CHECK(fixture.registry->DrainRetirements() == 0);
        CHECK(fixture.registry->HasPendingNativeRetirements());
        CHECK(fixture.budget->Snapshot().reservationCount == 1);
        CHECK(fixture.budget->Snapshot().reservedUnallocatedBytes == charged.reservedUnallocatedBytes);
        CHECK(fixture.budget->ReclaimEmptyBlocks(64) == 0);
        CHECK(fixture.destroyRequests == 0);
        // A host deadline or claimed shutdown disposition cannot fabricate lease completion.
        fixture.registry->Shutdown(BackendResourceReleaseMode::NativeAlreadyReleased);
        CHECK(fixture.registry->HasPendingNativeRetirements());
        CHECK(fixture.budget->Snapshot().reservationCount == 1);
        fixture.nativeBacking.reset();
        CHECK(fixture.budget->Snapshot().reservationCount == 1);  // Native callback only marks evidence.
        CHECK(fixture.registry->DrainRetirements() == 1);
        CHECK(fixture.budget->Snapshot().reservationCount == 0);
        CHECK_FALSE(fixture.registry->HasPendingNativeRetirements());
    }

    TEST_CASE("Published resource retirement preserves committed backing through deferred native destruction",
              "[unit][renderer][resource][retirement]") {
        Fixture fixture;
        fixture.Complete(Result<std::uint64_t>::Success(71));
        REQUIRE(fixture.budget->Snapshot().allocationCount == 1);
        REQUIRE(fixture.registry->Release(RenderResourceClass::Buffer, fixture.request.identity).HasValue());
        CHECK(fixture.registry->DrainRetirements() == 0);
        CHECK(fixture.destroyRequests == 1);
        CHECK(fixture.budget->Snapshot().allocationCount == 1);
        CHECK(fixture.budget->Snapshot().retiringPayloadBytes == 64);
        CHECK(fixture.registry->DrainRetirements() == 0);
        CHECK(fixture.destroyRequests == 1);
        fixture.nativeBacking.reset();
        CHECK(fixture.budget->Snapshot().allocationCount == 1);
        CHECK(fixture.registry->DrainRetirements() == 1);
        CHECK(fixture.budget->Snapshot().allocationCount == 0);
        CHECK(fixture.registry->DrainRetirements() == 0);
        CHECK(fixture.destroyRequests == 1);
        const auto next = fixture.registry->Reserve(RenderResourceClass::Buffer);
        REQUIRE(next.HasValue());
        CHECK(next.Value().identity.slot == fixture.request.identity.slot);
        CHECK(next.Value().identity.generation != fixture.request.identity.generation);
    }

    TEST_CASE("Publication rollback and lease moves retain one allocation and one native destruction request",
              "[unit][renderer][resource][retirement]") {
        Fixture fixture;
        auto moved = std::move(*fixture.nativeBacking);
        fixture.nativeBacking.reset();
        fixture.nativeBacking.emplace(std::move(moved));
        // Attach ownership before the publication boundary, then deliberately reject its malformed publication.
        REQUIRE(fixture.registry->AttachNativeCreation(RenderResourceClass::Buffer, fixture.request.identity, 71).HasValue());
        auto allocation = fixture.budget->Commit(fixture.request.memoryReservation);
        REQUIRE(allocation.HasValue());
        REQUIRE(fixture.registry->AttachNativeCreation(RenderResourceClass::Buffer, fixture.request.identity, 71, allocation.Value().id)
                    .HasValue());
        const auto rejected = fixture.registry->Publish(RenderResourceClass::Buffer, fixture.request.identity, 0, allocation.Value().id);
        REQUIRE(rejected.HasError());
        REQUIRE(fixture.registry->Fail(RenderResourceClass::Buffer, fixture.request.identity, rejected.ErrorValue()).HasValue());
        CHECK(fixture.registry->DrainRetirements() == 0);
        CHECK(fixture.budget->Snapshot().retiringPayloadBytes == 64);
        CHECK(fixture.destroyRequests == 1);
        fixture.nativeBacking.reset();
        CHECK(fixture.registry->DrainRetirements() == 1);
        CHECK(fixture.budget->Snapshot().allocationCount == 0);
        CHECK(fixture.budget->Snapshot().reservationCount == 0);
    }

    TEST_CASE("Native lease addresses remain stable while other bounded registry entries grow", "[unit][renderer][resource][retirement]") {
        Fixture fixture;
        for (int index = 0; index < 3; ++index)
            REQUIRE(fixture.registry->Reserve(RenderResourceClass::Texture).HasValue());
        fixture.Complete(Result<std::uint64_t>::Failure(Fixture::NativeFailure()));
        CHECK(fixture.registry->DrainRetirements() == 0);
        fixture.nativeBacking.reset();
        CHECK(fixture.registry->DrainRetirements() == 1);
        CHECK(fixture.budget->Snapshot().reservationCount == 0);
    }
}  // namespace Horo::Render::NativeRetirementTest
