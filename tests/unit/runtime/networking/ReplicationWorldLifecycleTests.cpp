#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "NetworkTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace Horo::Network {
    namespace {
        using TestSupport::RequireError;

        NetworkSessionGeneration Session(const std::uint64_t value = 1) {
            return NetworkSessionGeneration::Create(value).Value();
        }

        ReplicationAuthorityEpoch Authority(const std::uint64_t value = 1) {
            return ReplicationAuthorityEpoch::Create(value).Value();
        }

        ReplicationWorldActivationDescriptor World(const std::uint64_t scene = 10, const std::uint64_t session = 1,
                                                   const std::uint64_t authority = 1,
                                                   const ReplicationExecutionRole role = ReplicationExecutionRole::AuthorityServer) {
            return {Runtime::SceneRuntimeId{scene}, Session(session), Authority(authority), role,
                    ReplicationWorldPhaseSet::From({Runtime::RuntimePhase::NetworkPoll, Runtime::RuntimePhase::FixedUpdate,
                                                    Runtime::RuntimePhase::NetworkFlush,
                                                    Runtime::RuntimePhase::CommitDeferredLifecycleChanges})};
        }

        NetworkObjectMappingEntry Object(const std::uint64_t authority = 1, const std::uint64_t slot = 1,
                                         const std::uint32_t generation = 1, const std::uint64_t scene = 10,
                                         const std::uint32_t entityGeneration = 1) {
            return {NetworkObjectId::Create(Authority(authority), slot, generation).Value(),
                    Runtime::EntityRef{Runtime::SceneRuntimeId{scene},
                                       Runtime::EntityId{static_cast<std::uint32_t>(slot), entityGeneration}},
                    {ReplicationSchemaId::Create(7).Value(), {1, 0}, std::nullopt}};
        }

        ReplicationWorldWorkRequest FixedWork(const ReplicationWorldActivationDescriptor &world, const std::uint64_t tick = 1) {
            return {world.scene, world.session, Runtime::RuntimePhase::FixedUpdate, tick, {}};
        }
    }  // namespace

    TEST_CASE("Replication world publication is transactional and generation exact", "[unit][network][replication][lifecycle]") {
        auto lifecycle = std::move(ReplicationWorldLifecycle::Create({.maximumObjects = 4, .maximumRetiredWorlds = 1})).Value();
        const auto first = World();
        const auto replacement = World(11, 2, 2);

        REQUIRE(lifecycle.Stage(first).HasValue());
        RequireError(lifecycle.CommitAtSafePoint(first.scene, first.session, Runtime::RuntimePhase::NetworkFlush),
                     NetworkErrors::ReplicationWorldPhaseInvalid);
        REQUIRE(lifecycle.State() == ReplicationWorldLifecycleState::Empty);
        REQUIRE(lifecycle.CommitAtSafePoint(first.scene, first.session).HasValue());
        REQUIRE(lifecycle.ActiveDescriptor().Value() == first);

        auto oldLease = std::move(lifecycle.Acquire(FixedWork(first))).Value();
        REQUIRE(lifecycle.Stage(replacement).HasValue());
        RequireError(lifecycle.CommitAtSafePoint(first.scene, first.session), NetworkErrors::ReplicationWorldStale);
        REQUIRE(lifecycle.ActiveDescriptor().Value() == first);
        REQUIRE(oldLease.IsValid());
        REQUIRE(lifecycle.CommitAtSafePoint(replacement.scene, replacement.session).HasValue());
        REQUIRE(oldLease.IsRevoked());
        REQUIRE(oldLease.Cancellation().IsCancellationRequested());
        REQUIRE(lifecycle.ActiveDescriptor().Value() == replacement);
        RequireError(lifecycle.Acquire(FixedWork(first)), NetworkErrors::ReplicationWorldStale);
    }

    TEST_CASE("Replication world leases expose only declared phases and role capabilities", "[unit][network][replication][lifecycle]") {
        auto lifecycle = std::move(ReplicationWorldLifecycle::Create()).Value();
        const auto server = World();
        REQUIRE(lifecycle.Stage(server).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(server.scene, server.session).HasValue());

        REQUIRE(lifecycle.RegisterObject(server.scene, server.session, Object()).HasValue());
        auto capture = lifecycle.AcquireFor(FixedWork(server), ReplicationWorldCapability::CaptureCanonicalState);
        REQUIRE(capture.HasValue());
        REQUIRE(capture.Value().Capabilities().publishAuthority);
        REQUIRE(capture.Value().Mapping().Entries().size() == 1);
        RequireError(lifecycle.AcquireFor(FixedWork(server), ReplicationWorldCapability::SubmitCommands),
                     NetworkErrors::ReplicationAuthorityDenied);

        auto undeclared = FixedWork(server);
        undeclared.phase = Runtime::RuntimePhase::RenderExtraction;
        RequireError(lifecycle.Acquire(undeclared), NetworkErrors::ReplicationWorldPhaseInvalid);

        const auto client = World(12, 3, 3, ReplicationExecutionRole::AutonomousClient);
        REQUIRE(lifecycle.Stage(client).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(client.scene, client.session).HasValue());
        REQUIRE(lifecycle.AcquireFor(FixedWork(client), ReplicationWorldCapability::SubmitCommands).HasValue());
        REQUIRE(lifecycle.AcquireFor(FixedWork(client), ReplicationWorldCapability::ApplyAuthoritativeState).HasValue());
        RequireError(lifecycle.AcquireFor(FixedWork(client), ReplicationWorldCapability::PublishAuthority),
                     NetworkErrors::ReplicationAuthorityDenied);
    }

    TEST_CASE("Replication world pause, cancellation, loss, duplication, and stale object identity are bounded",
              "[unit][network][replication][lifecycle]") {
        auto lifecycle = std::move(ReplicationWorldLifecycle::Create({.maximumObjects = 2, .maximumRetiredWorlds = 2})).Value();
        const auto world = World();
        REQUIRE(lifecycle.Stage(world).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());

        const auto object = Object();
        REQUIRE(lifecycle.RegisterObject(world.scene, world.session, object).HasValue());
        RequireError(lifecycle.RegisterObject(world.scene, world.session, object), NetworkErrors::NetworkObjectMappingConflict);
        REQUIRE(lifecycle.RetireObject(world.scene, world.session, object.object).HasValue());
        RequireError(lifecycle.RetireObject(world.scene, world.session, object.object), NetworkErrors::NetworkObjectMappingUnknown);
        REQUIRE(lifecycle.RegisterObject(world.scene, world.session, Object(1, 1, 2)).HasValue());
        RequireError(lifecycle.RegisterObject(world.scene, world.session, Object(1, 1, 1)), NetworkErrors::NetworkObjectMappingConflict);

        REQUIRE(lifecycle.Pause(world.scene, world.session).HasValue());
        RequireError(lifecycle.Acquire(FixedWork(world)), NetworkErrors::ReplicationWorldUnavailable);
        REQUIRE(lifecycle.Resume(world.scene, world.session).HasValue());

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto cancelled = FixedWork(world);
        cancelled.cancellation = cancellation.Token();
        RequireError(lifecycle.Acquire(cancelled), NetworkErrors::ReplicationWorldCancelled);

        auto lease = std::move(lifecycle.Acquire(FixedWork(world))).Value();
        lifecycle.BeginShutdown();
        REQUIRE(lifecycle.State() == ReplicationWorldLifecycleState::ShuttingDown);
        REQUIRE(lease.IsRevoked());
        RequireError(lifecycle.Acquire(FixedWork(world)), NetworkErrors::ReplicationWorldShuttingDown);
        REQUIRE(lifecycle.CollectRetired() == ReplicationWorldLifecycleState::ShuttingDown);
        lease = {};
        REQUIRE(lifecycle.CollectRetired() == ReplicationWorldLifecycleState::Closed);
        lifecycle.BeginShutdown();
    }

    TEST_CASE("Replication world cancellation prevents partial activation and shutdown rejects late candidates",
              "[unit][network][replication][lifecycle]") {
        auto lifecycle = std::move(ReplicationWorldLifecycle::Create()).Value();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(lifecycle.Stage(World(), cancellation.Token()), NetworkErrors::ReplicationWorldCancelled);
        REQUIRE(lifecycle.State() == ReplicationWorldLifecycleState::Empty);
        REQUIRE_FALSE(lifecycle.HasStagedCandidate());

        const auto valid = World();
        REQUIRE(lifecycle.Stage(valid).HasValue());
        lifecycle.BeginShutdown();
        RequireError(lifecycle.CommitAtSafePoint(valid.scene, valid.session), NetworkErrors::ReplicationWorldShuttingDown);
        REQUIRE(lifecycle.State() == ReplicationWorldLifecycleState::Closed);
    }

    TEST_CASE("Replication world validates owner identity across mutation and retirement paths",
              "[unit][network][replication][lifecycle]") {
        RequireError(ReplicationWorldLifecycle::Create({.maximumObjects = 0, .maximumRetiredWorlds = 1}),
                     NetworkErrors::ReplicationWorldInvalid);
        RequireError(ReplicationWorldLifecycle::Create(
                         {.maximumObjects = 1, .maximumRetiredWorlds = ReplicationWorldLifecycle::MaximumRetiredWorlds + 1}),
                     NetworkErrors::ReplicationWorldInvalid);

        auto lifecycle = std::move(ReplicationWorldLifecycle::Create({.maximumObjects = 2, .maximumRetiredWorlds = 2})).Value();
        RequireError(lifecycle.ActiveDescriptor(), NetworkErrors::ReplicationWorldUnavailable);

        auto invalid = World();
        invalid.scene = {};
        RequireError(lifecycle.Stage(invalid), NetworkErrors::ReplicationWorldInvalid);

        const auto world = World();
        REQUIRE(lifecycle.Stage(world).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        RequireError(lifecycle.Pause({}, world.session), NetworkErrors::ReplicationWorldInvalid);
        RequireError(lifecycle.Pause(Runtime::SceneRuntimeId{99}, world.session), NetworkErrors::ReplicationWorldStale);
        RequireError(lifecycle.Resume(world.scene, world.session), NetworkErrors::ReplicationWorldUnavailable);
        RequireError(lifecycle.RegisterObject({}, world.session, Object()), NetworkErrors::ReplicationWorldInvalid);
        RequireError(lifecycle.RetireObject(Runtime::SceneRuntimeId{99}, world.session, Object().object),
                     NetworkErrors::ReplicationWorldStale);

        auto zeroTick = FixedWork(world, 0);
        RequireError(lifecycle.Acquire(zeroTick), NetworkErrors::ReplicationWorldInvalid);
        REQUIRE(lifecycle.Pause(world.scene, world.session).HasValue());
        REQUIRE(lifecycle.Pause(world.scene, world.session).HasValue());
        REQUIRE(lifecycle.Resume(world.scene, world.session).HasValue());

        const auto replacement = World(11, 2, 2, ReplicationExecutionRole::SimulatedClient);
        REQUIRE(lifecycle.Stage(replacement).HasValue());
        REQUIRE(lifecycle.Revoke(world.scene, world.session).HasValue());
        REQUIRE_FALSE(lifecycle.HasStagedCandidate());
        REQUIRE(lifecycle.State() == ReplicationWorldLifecycleState::Empty);

        REQUIRE(lifecycle.Stage(replacement).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(replacement.scene, replacement.session).HasValue());
        REQUIRE(lifecycle.AcquireFor(FixedWork(replacement), ReplicationWorldCapability::ApplyAuthoritativeState).HasValue());
        RequireError(lifecycle.AcquireFor(FixedWork(replacement), ReplicationWorldCapability::SubmitCommands),
                     NetworkErrors::ReplicationAuthorityDenied);
        REQUIRE(lifecycle.Unload(replacement.scene, replacement.session).HasValue());
        REQUIRE(lifecycle.State() == ReplicationWorldLifecycleState::Empty);
    }
}  // namespace Horo::Network
