#include "Horo/Runtime/Render/RenderMemoryBudgetErrors.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderTransientGraphTestSupport.h"
#include "RenderTransientTestBackend.h"

#include <new>

namespace Horo::Render::TransientReservationTest {
    using namespace Detail;
    using namespace TransientTest;

    /** @brief Creates a real one-slot budget whose hard cap rejects the graph before native creation. */
    [[nodiscard]] std::unique_ptr<RenderMemoryBudget> DenyingBudget(const RenderResourceOwnerId owner) {
        auto created = RenderMemoryBudget::Create(owner, {.hardCapBytes = 8,
                                                          .defaultBlockBytes = 8,
                                                          .maximumBlockBytes = 8,
                                                          .maximumAlignment = 8,
                                                          .maximumPools = 1,
                                                          .maximumBlocks = 1,
                                                          .maximumReservations = 1,
                                                          .maximumAllocations = 1});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    TEST_CASE("Transient admission metadata allocation failures preserve the provisional registry claim",
              "[renderer][transient][rollback]") {
        auto sources = CopyGraph();
        bool allocationFailureObserved = false;
        for (std::size_t position = 0; position < 32; ++position) {
            const auto owner = AcquireRenderResourceOwnerId();
            REQUIRE(owner.HasValue());
            auto audit = std::make_shared<Audit>();
            auto createdBackend = ProbeProvider{audit}.Create();
            REQUIRE(createdBackend.HasValue());
            auto backend = std::move(createdBackend).Value();
            REQUIRE(backend->Initialize({}).HasValue());
            auto budget = DenyingBudget(owner.Value());
            RenderResourceRegistry registry{owner.Value(), {.maximumSlots = 1, .maximumPendingRequests = 1}};
            RenderGraphTransientResourcePool pool{*backend, registry, *budget};
            {
                Tests::AllocationProbe::ScopedFailure failure{position};
                try {
                    static_cast<void>(pool.Prepare(sources.lifetime, {7, 1}));
                } catch (const std::bad_alloc &) {
                    allocationFailureObserved = true;
                }
            }
            const auto retried = pool.Prepare(sources.lifetime, {7, 1});
            REQUIRE(retried.HasError());
            if (retried.ErrorValue().code.Value() == RenderMemoryBudgetErrors::BudgetExceeded.code.Value()) {
                REQUIRE(registry.Reserve(RenderResourceClass::Buffer).HasValue());
            } else {
                REQUIRE(retried.ErrorValue().code.Value() == "render.frontend.resource.backend_exception");
                CHECK(retried.ErrorValue().message.find("shut down") != std::string::npos);
            }
            {
                Tests::AllocationProbe::ScopedFailure failure;
                registry.Shutdown(BackendResourceReleaseMode::DestroyNative);
            }
            CHECK(audit->resources.creates == 0);
            CHECK(budget->Snapshot().reservationCount == 0);
            CHECK(budget->Snapshot().allocationCount == 0);
            CHECK(budget->Snapshot().reservedUnallocatedBytes == 0);
        }
        REQUIRE(allocationFailureObserved);
    }
}  // namespace Horo::Render::TransientReservationTest
