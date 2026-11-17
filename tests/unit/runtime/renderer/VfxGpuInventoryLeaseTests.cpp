#include "RenderResourceRegistry.h"
#include "VfxGpuInventoryLease.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    // Registry-only lifetime tests; the published numbers are not native GPU preparation evidence.
    struct RegistryFixture {
        std::size_t released{};
        RenderResourceRegistry registry;
        std::shared_ptr<const VfxFrontendState> authority{std::make_shared<const VfxFrontendState>()};

        RegistryFixture()
            : registry({901},
                       {.maximumSlots = 32,
                        .maximumPendingRequests = 32,
                        .retirementDrainBudget = 64,
                        .maximumOperationResults = 64,
                        .maximumSubmissionPins = 64,
                        .maximumTrackedQueues = 1,
                        .completionDrainBudget = 16},
                       [this](RenderResourceClass, std::uint64_t, std::optional<RenderMemoryAllocationId>, BackendResourceReleaseMode) {
                           ++released;
                       }) {}

        ~RegistryFixture() {
            registry.Shutdown(BackendResourceReleaseMode::DestroyNative);
        }

        RenderBufferHandle Buffer() {
            auto reserved = registry.Reserve(RenderResourceClass::Buffer);
            REQUIRE(reserved.HasValue());
            const auto id = reserved.Value().identity;
            REQUIRE(registry.Publish(RenderResourceClass::Buffer, id, id.slot).HasValue());
            return {id.owner, id.slot, id.generation};
        }

        void Release(const RenderBufferHandle buffer) {
            REQUIRE(registry.Release(RenderResourceClass::Buffer, {buffer.owner, buffer.slot, buffer.generation}).HasValue());
            static_cast<void>(registry.DrainRetirements());
        }
    };
}  // namespace

TEST_CASE("GPU inventory pins retain logically released buffers until simulation retirement", "[renderer][vfx][inventory]") {
    RegistryFixture fixture;
    const auto buffer = fixture.Buffer();
    const std::array buffers{buffer};
    auto prepared = VfxGpuInventoryLease::Create(fixture.registry, buffers, fixture.authority);
    REQUIRE(prepared.HasValue());
    auto lease = std::move(prepared).Value();
    fixture.Release(buffer);
    CHECK(fixture.released == 0);
    lease->Release();
    CHECK(fixture.registry.DrainRetirements() == 1);
    CHECK(fixture.released == 1);
    lease->Release();
    lease.reset();
    CHECK(fixture.registry.DrainRetirements() == 0);
    CHECK(fixture.released == 1);
}

TEST_CASE("GPU cold inventories do not consume the eight frame-completion lease slots", "[renderer][vfx][inventory]") {
    RegistryFixture fixture;
    std::array<std::shared_ptr<VfxGpuInventoryLease>, 9> leases;
    for (auto &lease : leases) {
        const std::array buffers{fixture.Buffer()};
        auto prepared = VfxGpuInventoryLease::Create(fixture.registry, buffers, fixture.authority);
        REQUIRE(prepared.HasValue());
        lease = std::move(prepared).Value();
        fixture.Release(buffers[0]);
    }
    CHECK(fixture.released == 0);
    for (auto &lease : leases)
        lease.reset();
    CHECK(fixture.registry.DrainRetirements() == leases.size());
    CHECK(fixture.released == leases.size());
}

TEST_CASE("GPU inventory acquisition rolls back prefix pins on malformed and duplicate input", "[renderer][vfx][inventory]") {
    RegistryFixture fixture;
    const auto buffer = fixture.Buffer();
    std::array buffers{buffer, buffer};
    SECTION("foreign owner") {
        buffers[1].owner = {902};
    }
    SECTION("stale generation") {
        ++buffers[1].generation;
    }
    SECTION("duplicate exact generation") {}
    auto prepared = VfxGpuInventoryLease::Create(fixture.registry, buffers, fixture.authority);
    CHECK(prepared.HasError());
    fixture.Release(buffer);
    CHECK(fixture.released == 1);
}

TEST_CASE("Revoked GPU inventories cannot access a destroyed registry", "[renderer][vfx][inventory]") {
    std::shared_ptr<VfxGpuInventoryLease> retained;
    {
        RegistryFixture fixture;
        const std::array buffers{fixture.Buffer()};
        auto prepared = VfxGpuInventoryLease::Create(fixture.registry, buffers, fixture.authority);
        REQUIRE(prepared.HasValue());
        retained = std::move(prepared).Value();
        fixture.authority.reset();
        fixture.registry.Shutdown(BackendResourceReleaseMode::NativeAlreadyReleased);
        CHECK(fixture.released == 1);
    }
    retained->Release();
    retained.reset();
    SUCCEED("Retirement followed frontend revocation without touching the dead registry.");
}
