#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>
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
            descriptor.geometry = PhysicsRayQuery{.maximumDistanceMeters = 10};
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
        REQUIRE(read.Value().omittedRecordCount == 0);
        REQUIRE(read.Value().droppedRecordCount == 0);
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

    TEST_CASE("Query/event access rejects structural edits, unloaded fixtures and foreign threads", "[physics][query-event-capability]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(55).Value()).HasValue());
        auto capability = world->IssueQueryEventCapability().Value();
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto before = world->PublishedTick();
        std::array<PhysicsEventRecord, 1> events{};
        std::array<PhysicsQueryHit, 1> hits{};

        bool rejectedThread{};
        std::thread reader([&] {
            const auto read = capability.ReadEvents(EventsAt(capability, before), events);
            rejectedThread = read.HasError() && read.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        reader.join();
        REQUIRE(rejectedThread);

        const auto fixture =
            world
                ->CreateQueryFixture({.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                      .pose = {.translation = {0, 0, -5}, .rotation = Math::Quaternion::Identity()},
                                      .layer = CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                                      .profile = CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value(),
                                      .channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value()})
                .Value();
        const auto admitted = world->PublishedTick();
        REQUIRE(admitted.publicationRevision > before.publicationRevision);
        REQUIRE(admitted.eventTick == 0);
        Test::RequireError(capability.Submit(RayAt(capability, before), hits), PhysicsErrors::QuerySnapshotStale);
        Test::RequireError(capability.ReadEvents(EventsAt(capability, before), events), PhysicsErrors::QuerySnapshotStale);
        const auto query = capability.Submit(RayAt(capability, admitted), hits);
        REQUIRE(query.HasValue());
        REQUIRE(query.Value().result.hitCount == 1);
        REQUIRE(hits[0].body == fixture.body);

        REQUIRE(world->DestroyQueryFixture(fixture).HasValue());
        const auto removed = world->PublishedTick();
        REQUIRE(removed.publicationRevision > admitted.publicationRevision);
        Test::RequireError(capability.Submit(RayAt(capability, admitted), hits), PhysicsErrors::QuerySnapshotStale);
        const auto empty = capability.Submit(RayAt(capability, removed), hits);
        REQUIRE(empty.HasValue());
        REQUIRE(empty.Value().result.hitCount == 0);
        Test::RequireError(world->DestroyQueryFixture(fixture), PhysicsErrors::HandleStale);
        REQUIRE(world->PublishedTick().publicationRevision == removed.publicationRevision);

        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto republished = world->PublishedTick();
        REQUIRE(republished.eventTick == 2);
        REQUIRE(republished.publicationRevision > removed.publicationRevision);
        REQUIRE(capability.ReadEvents(EventsAt(capability, republished), events).HasValue());

        REQUIRE(world->UnloadScene().HasValue());
        world.reset();
        Test::RequireError(capability.ReadEvents(EventsAt(capability, removed), events), PhysicsErrors::CapabilityStale);
        Test::RequireError(capability.Submit(RayAt(capability, removed), hits), PhysicsErrors::CapabilityStale);
    }

    TEST_CASE("Queued Physics queries publish ordered owned hits only after owner processing", "[physics][query-batch]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(56).Value()).HasValue());
        const auto layer = CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        const auto profile = CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value();
        const auto channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        const auto first = world
                               ->CreateQueryFixture({.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                                     .pose = {.translation = {0, 0, -5}, .rotation = Math::Quaternion::Identity()},
                                                     .layer = layer,
                                                     .profile = profile,
                                                     .channel = channel})
                               .Value();
        const auto second = world
                                ->CreateQueryFixture({.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                                      .pose = {.translation = {0, 0, -8}, .rotation = Math::Quaternion::Identity()},
                                                      .layer = layer,
                                                      .profile = profile,
                                                      .channel = channel})
                                .Value();
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        auto capability = world->IssueQueryEventCapability().Value();
        const auto publication = world->PublishedTick();
        std::array commands{RayAt(capability, publication), RayAt(capability, publication)};
        std::get<PhysicsRayQuery>(commands[0].descriptor.geometry).maximumDistanceMeters = 6;
        std::get<PhysicsRayQuery>(commands[1].descriptor.geometry).origin = {0, 0, -6};
        std::get<PhysicsRayQuery>(commands[1].descriptor.geometry).maximumDistanceMeters = 4;
        auto batch = capability.SubmitBatch(commands).Value();
        REQUIRE(batch.Poll().HasValue());
        REQUIRE_FALSE(batch.Poll().Value());
        Test::RequireError(capability.SubmitBatch(commands), PhysicsErrors::CapacityExceeded);
        bool foreignPumpRejected{};
        std::thread worker([&] {
            const auto result = world->ProcessQueryBatch();
            foreignPumpRejected =
                result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        worker.join();
        REQUIRE(foreignPumpRejected);
        bool foreignSubmitRejected{};
        std::thread foreignSubmit([&] {
            const auto result = capability.SubmitBatch(commands);
            foreignSubmitRejected =
                result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreignSubmit.join();
        REQUIRE(foreignSubmitRejected);
        REQUIRE(world->ProcessQueryBatch().HasValue());
        const auto result = batch.Poll();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value());
        REQUIRE(result.Value()->entries.size() == 2);
        REQUIRE(result.Value()->entries[0].hits.size() == 1);
        REQUIRE(result.Value()->entries[1].hits.size() == 1);
        REQUIRE(result.Value()->entries[0].hits[0].body == first.body);
        REQUIRE(result.Value()->entries[1].hits[0].body == second.body);
        REQUIRE(result.Value()->entries[0].completion.publicationRevision == publication.publicationRevision);
        REQUIRE_FALSE(batch.Cancel());
        REQUIRE(batch.Poll().Value()->entries.size() == 2);
        REQUIRE(world->UnloadScene().HasValue());
        world.reset();
        REQUIRE(batch.Poll().Value()->entries[0].hits[0].body == first.body);
    }

    TEST_CASE("Queued Physics queries cancel, stale and bound all requested results", "[physics][query-batch]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(57).Value()).HasValue());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        auto capability = world->IssueQueryEventCapability().Value();
        const auto publication = world->PublishedTick();
        std::array commands{RayAt(capability, publication), RayAt(capability, publication)};
        auto cancelled = capability.SubmitBatch(commands).Value();
        bool workerCancelled{};
        std::thread worker([&] {
            workerCancelled = cancelled.Cancel();
        });
        worker.join();
        REQUIRE(workerCancelled);
        Test::RequireError(cancelled.Poll(), PhysicsErrors::QueryCancelled);
        REQUIRE(world->ProcessQueryBatch().HasValue());
        REQUIRE_FALSE(cancelled.Cancel());

        auto stale = capability.SubmitBatch(commands).Value();
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        REQUIRE(world->ProcessQueryBatch().HasValue());
        Test::RequireError(stale.Poll(), PhysicsErrors::QuerySnapshotStale);
        Test::RequireError(capability.SubmitBatch(commands), PhysicsErrors::QuerySnapshotStale);

        auto structurallyStale = capability.SubmitBatch(std::array{RayAt(capability, world->PublishedTick())}).Value();
        REQUIRE(world
                    ->CreateQueryFixture({.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                          .pose = {.translation = {0, 0, -5}, .rotation = Math::Quaternion::Identity()},
                                          .layer = CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                                          .profile = CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value(),
                                          .channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value()})
                    .HasValue());
        REQUIRE(world->ProcessQueryBatch().HasValue());
        Test::RequireError(structurallyStale.Poll(), PhysicsErrors::QuerySnapshotStale);

        const auto current = world->PublishedTick();
        commands = {RayAt(capability, current), RayAt(capability, current)};
        auto multiHit = RayAt(capability, current);
        multiHit.descriptor.collection = PhysicsQueryCollection::All;
        multiHit.descriptor.maximumHitCount = MaximumPhysicsQueryHits;
        std::array acceptedHits{multiHit, multiHit, multiHit, multiHit};
        REQUIRE(acceptedHits.size() * MaximumPhysicsQueryHits == MaximumPhysicsQueryBatchHits);
        auto accepted = capability.SubmitBatch(acceptedHits).Value();
        REQUIRE(world->ProcessQueryBatch().HasValue());
        REQUIRE(accepted.Poll().HasValue());
        REQUIRE(accepted.Poll().Value()->entries.size() == acceptedHits.size());
        std::array tooManyHits{multiHit, multiHit, multiHit, multiHit, multiHit};
        Test::RequireError(capability.SubmitBatch(tooManyHits), PhysicsErrors::CapacityExceeded);
        const std::vector<PhysicsQueryCommand> tooManyQueries(world->Settings().Values().budgets.maximumQueriesPerTick + 1,
                                                              RayAt(capability, current));
        Test::RequireError(capability.SubmitBatch(tooManyQueries), PhysicsErrors::CapacityExceeded);
        std::array<PhysicsQueryCommand, 0> empty{};
        Test::RequireError(capability.SubmitBatch(empty), PhysicsErrors::CapacityExceeded);

        auto retired = capability.SubmitBatch(commands).Value();
        REQUIRE(world->Reset().HasValue());
        Test::RequireError(retired.Poll(), PhysicsErrors::CapabilityStale);
        REQUIRE(world->ProcessQueryBatch().HasValue());
    }

    TEST_CASE("Revocation and shutdown terminate queued Physics batches without native retention", "[physics][query-batch]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(58).Value()).HasValue());
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        auto capability = world->IssueQueryEventCapability().Value();
        auto command = RayAt(capability, world->PublishedTick());
        auto revoked = capability.SubmitBatch(std::span{&command, 1}).Value();
        REQUIRE(world->RevokeQueryEventCapability(capability).HasValue());
        Test::RequireError(revoked.Poll(), PhysicsErrors::CapabilityRevoked);
        REQUIRE(world->ProcessQueryBatch().HasValue());

        capability = world->IssueQueryEventCapability().Value();
        command = RayAt(capability, world->PublishedTick());
        auto stopped = capability.SubmitBatch(std::span{&command, 1}).Value();
        world->Shutdown();
        world.reset();
        runtime.reset();
        Test::RequireError(stopped.Poll(), PhysicsErrors::CapabilityStale);
        REQUIRE_FALSE(stopped.Cancel());
    }
#endif

    static_assert(std::is_copy_constructible_v<PhysicsQueryEventCapability>);
    static_assert(std::is_same_v<decltype(PhysicsQueryBatchEntry::hits)::value_type, PhysicsQueryHit>);
    static_assert(std::is_trivially_copyable_v<PhysicsEventRecord>);
}  // namespace Horo::Physics
