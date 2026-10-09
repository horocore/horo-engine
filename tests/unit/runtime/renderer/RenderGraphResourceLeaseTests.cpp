#include "RenderGraphResourceLeasePool.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::Detail {
    struct ResourceLeaseFixture {
        std::size_t destroyed = 0;
        RenderResourceRegistry registry{{1},
                                        {.maximumSubmissionPins = 2},
                                        [this](RenderResourceClass, std::uint64_t, std::optional<RenderMemoryAllocationId>,
                                               BackendResourceReleaseMode) {
            ++destroyed;
        }};
        RenderResourceIdentity identity;
        RenderGraphResource resource;
        RenderGraphResourceLeasePool pool{registry, 2};

        ResourceLeaseFixture() {
            const auto reserved = registry.Reserve(RenderResourceClass::Buffer);
            REQUIRE(reserved.HasValue());
            identity = reserved.Value().identity;
            REQUIRE(registry.Publish(RenderResourceClass::Buffer, identity, 41).HasValue());
            const RenderBufferHandle handle{identity.owner, identity.slot, identity.generation};
            resource = {{{1}, 1}, RenderGraphResourceKind::Buffer, RenderGraphResourceClass::Persistent, handle};
        }
    };

    TEST_CASE_METHOD(ResourceLeaseFixture, "Graph resource leases preserve retiring residents and roll back rejected acquisition",
                     "[renderer][render-graph][resource]") {
        SECTION("completion releases resident exactly once") {
            const std::array resources{resource};
            const auto leased = pool.Acquire(resources);
            REQUIRE(leased.HasValue());
            REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasValue());
            REQUIRE(registry.DrainRetirements() == 0);
            REQUIRE(destroyed == 0);
            leased.Value()->Release();
            leased.Value()->Release();
            REQUIRE(registry.DrainRetirements() == 1);
            REQUIRE(destroyed == 1);
        }
        SECTION("stale trailing generation rolls back an already pinned prefix") {
            auto stale = resource;
            ++std::get<RenderBufferHandle>(stale.binding).generation;
            const std::array resources{resource, stale};
            REQUIRE(pool.Acquire(resources).HasError());
            REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasValue());
            REQUIRE(registry.DrainRetirements() == 1);
            REQUIRE(destroyed == 1);
        }
        SECTION("bounded pin capacity rejects without changing residents") {
            const std::array resources{resource, resource, resource};
            REQUIRE(pool.Acquire(resources).HasError());
            REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasValue());
            REQUIRE(registry.DrainRetirements() == 1);
        }
        SECTION("independent submissions preserve every outstanding use") {
            const std::array resources{resource};
            const auto first = pool.Acquire(resources);
            const auto second = pool.Acquire(resources);
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(pool.Acquire(resources).HasError());
            REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasValue());
            first.Value()->Release();
            REQUIRE(registry.DrainRetirements() == 0);
            second.Value()->Release();
            REQUIRE(registry.DrainRetirements() == 1);
        }
    }

    TEST_CASE_METHOD(ResourceLeaseFixture, "Graph leases share the bounded pin budget with timeline submissions",
                     "[renderer][render-graph][resource]") {
        REQUIRE(registry.TrackSubmission(RenderResourceClass::Buffer, identity, {{1}, 1}).HasValue());
        const std::array resources{resource, resource};
        REQUIRE(pool.Acquire(resources).HasError());
        REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasValue());
        REQUIRE(registry.DrainRetirements() == 0);
        REQUIRE(registry.AcknowledgeCompletion({{1}, 1}).Value() == 1);
        REQUIRE(registry.DrainRetirements() == 1);
    }
}  // namespace Horo::Render::Detail
