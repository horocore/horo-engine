#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Physics {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        [[nodiscard]] PhysicsEventReadCommand EventsAt(const PhysicsQueryEventCapability &capability, const PhysicsPublishedTick tick,
                                                       const std::uint32_t limit = 1) {
            return {.identity = capability.Identity(),
                    .completedTick = tick.eventTick,
                    .publicationRevision = tick.publicationRevision,
                    .maximumRecords = limit};
        }

        [[nodiscard]] PhysicsQueryCommand RayAt(const PhysicsQueryEventCapability &capability, const PhysicsPublishedTick tick) {
            PhysicsQueryDescriptor descriptor;
            descriptor.world = capability.Identity().world;
            descriptor.sceneGeneration = 7;
            descriptor.geometry = PhysicsRayQuery{};
            descriptor.filter.channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
            return {.identity = capability.Identity(), .expectedPublicationRevision = tick.publicationRevision, .descriptor = descriptor};
        }

        struct CallbackProbe final {
            const PhysicsQueryEventCapability *capability{};
            bool rejected{};
        };

        void ReadDuringTick(void *context, const PhysicsTickPhase, const std::uint64_t) noexcept {
            auto &probe = *static_cast<CallbackProbe *>(context);
            std::array<PhysicsEventRecord, 1> records{};
            const auto read = probe.capability->ReadEvents({.identity = probe.capability->Identity(),
                                                            .completedTick = 1,
                                                            .publicationRevision = 1,
                                                            .maximumRecords = 1},
                                                           records);
            probe.rejected = read.HasError() && read.ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value();
        }
    }  // namespace
#endif

    TEST_CASE("Null Physics cannot issue query/event access", "[physics][query-event-capability]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(50).Value()).HasValue());
        Test::RequireError(world->IssueQueryEventCapability(), PhysicsErrors::CapabilityUnavailable);
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Query/event capability validates identity, completion, bounds and revocation", "[physics][query-event-capability]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(51).Value()).HasValue());
        auto capability = world->IssueQueryEventCapability().Value();
        auto copy = capability;
        auto movable = world->IssueQueryEventCapability().Value();
        auto moved = std::move(movable);
        REQUIRE_FALSE(movable.Identity().world.IsValid());
        REQUIRE(movable.Identity().capabilityGeneration == 0);
        std::array<PhysicsEventRecord, 1> events{};
        std::array<PhysicsQueryHit, 1> hits{};
        Test::RequireError(movable.ReadEvents(EventsAt(movable, world->PublishedTick()), events), PhysicsErrors::CapabilityStale);
        Test::RequireError(movable.Submit(RayAt(movable, world->PublishedTick()), hits), PhysicsErrors::CapabilityStale);
        Test::RequireError(world->RevokeQueryEventCapability(movable), PhysicsErrors::CapabilityStale);
        REQUIRE(world->RevokeQueryEventCapability(moved).HasValue());
        Test::RequireError(capability.ReadEvents(EventsAt(capability, world->PublishedTick()), events),
                           PhysicsErrors::CapabilityUnavailable);
        Test::RequireError(capability.Submit(RayAt(capability, world->PublishedTick()), hits), PhysicsErrors::CapabilityUnavailable);

        const auto tick = Duration::FromNanoseconds(16'666'667);
        CallbackProbe probe{.capability = &capability};
        REQUIRE(world
                    ->AdvanceFixedTick({.simulationTick = 1,
                                        .sceneGeneration = 7,
                                        .fixedDelta = tick,
                                        .observer = {.context = &probe, .phase = ReadDuringTick}})
                    .HasValue());
        REQUIRE(probe.rejected);
        const auto published = world->PublishedTick();
        REQUIRE(published.eventTick == 1);
        const auto read = capability.ReadEvents(EventsAt(capability, published), events);
        REQUIRE(read.HasValue());
        REQUIRE(read.Value().recordCount == 0);
        REQUIRE_FALSE(read.Value().truncated);
        const auto query = capability.Submit(RayAt(capability, published), hits);
        REQUIRE(query.HasValue());
        REQUIRE(query.Value().completedTick == 1);
        REQUIRE(query.Value().publicationRevision == published.publicationRevision);

        auto foreign = EventsAt(capability, published);
        foreign.identity.world = PhysicsWorldId::Create(52).Value();
        Test::RequireError(capability.ReadEvents(foreign, events), PhysicsErrors::HandleWorldMismatch);
        foreign = EventsAt(capability, published);
        ++foreign.identity.capabilityGeneration;
        Test::RequireError(capability.ReadEvents(foreign, events), PhysicsErrors::CapabilityStale);
        foreign = EventsAt(capability, published);
        foreign.maximumRecords = 0;
        Test::RequireError(capability.ReadEvents(foreign, events), PhysicsErrors::CapacityExceeded);
        foreign.maximumRecords = world->Settings().Values().budgets.maximumEvents + 1;
        Test::RequireError(capability.ReadEvents(foreign, events), PhysicsErrors::CapacityExceeded);
        auto foreignQuery = RayAt(capability, published);
        foreignQuery.descriptor.world = PhysicsWorldId::Create(52).Value();
        Test::RequireError(capability.Submit(foreignQuery, hits), PhysicsErrors::HandleWorldMismatch);

        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = tick}).HasValue());
        Test::RequireError(capability.ReadEvents(EventsAt(capability, published), events), PhysicsErrors::QuerySnapshotStale);
        Test::RequireError(capability.Submit(RayAt(capability, published), hits), PhysicsErrors::QuerySnapshotStale);
        REQUIRE(world->RevokeQueryEventCapability(capability).HasValue());
        Test::RequireError(copy.ReadEvents(EventsAt(copy, world->PublishedTick()), events), PhysicsErrors::CapabilityRevoked);
        Test::RequireError(copy.Submit(RayAt(copy, world->PublishedTick()), hits), PhysicsErrors::CapabilityRevoked);

        auto replacement = world->IssueQueryEventCapability().Value();
        REQUIRE(replacement.Identity().capabilityGeneration != capability.Identity().capabilityGeneration);
        REQUIRE(world->Reset().HasValue());
        Test::RequireError(replacement.ReadEvents(EventsAt(replacement, published), events), PhysicsErrors::CapabilityStale);
        REQUIRE(world->Activate(PhysicsWorldId::Create(53).Value()).HasValue());
        auto newCapability = world->IssueQueryEventCapability().Value();
        REQUIRE(newCapability.Identity().world != replacement.Identity().world);
        REQUIRE(world->UnloadScene().HasValue());
        world.reset();
        Test::RequireError(newCapability.ReadEvents(EventsAt(newCapability, published), events), PhysicsErrors::CapabilityStale);
    }

    TEST_CASE("Query/event capability admission is finite and shutdown invalidates retained copies", "[physics][query-event-capability]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(54).Value()).HasValue());
        std::vector<PhysicsQueryEventCapability> issued;
        issued.reserve(MaximumPhysicsQueryEventCapabilitiesPerWorld);
        for (std::uint32_t index = 0; index < MaximumPhysicsQueryEventCapabilitiesPerWorld; ++index)
            issued.push_back(world->IssueQueryEventCapability().Value());
        Test::RequireError(world->IssueQueryEventCapability(), PhysicsErrors::CapacityExceeded);
        REQUIRE(world->RevokeQueryEventCapability(issued.front()).HasValue());
        auto replacement = world->IssueQueryEventCapability();
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value().Identity().capabilityGeneration > issued.back().Identity().capabilityGeneration);
        world->Shutdown();
        world.reset();
        std::array<PhysicsEventRecord, 1> events{};
        Test::RequireError(issued.back().ReadEvents(EventsAt(issued.back(), {}), events), PhysicsErrors::CapabilityStale);
        Test::RequireError(replacement.Value().ReadEvents(EventsAt(replacement.Value(), {}), events), PhysicsErrors::CapabilityStale);
    }
#endif

    static_assert(std::is_copy_constructible_v<PhysicsQueryEventCapability>);
    static_assert(std::is_trivially_copyable_v<PhysicsEventRecord>);
}  // namespace Horo::Physics
