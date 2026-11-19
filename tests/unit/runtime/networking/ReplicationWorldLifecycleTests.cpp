#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Network/NetworkAuthoredScenePage.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetrics.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "NetworkTestUtils.h"

#include <array>
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

        ReplicationWorldWorkRequest StructuralWork(const ReplicationWorldActivationDescriptor &world, CancellationToken token = {}) {
            return {world.scene, world.session, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, 1, std::move(token)};
        }

        /** @brief Supplies two object identities to the real Scene group transaction from its detached member handles. */
        class GroupMappingParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            GroupMappingParticipant(ReplicationWorldLifecycle &world, ReplicationWorldActivationDescriptor descriptor)
                : world_(world), descriptor_(descriptor) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &active,
                                                                               std::span<const Runtime::RuntimeEntityView> created,
                                                                               std::span<const Runtime::EntityRef> destroyed) override {
                REQUIRE(active.RuntimeId() == descriptor_.scene);
                if (created.empty()) {
                    REQUIRE(destroyed.size() == 2);
                    const std::array retired{Object(1, 1).object, Object(1, 2).object};
                    return world_.PrepareObjectBatch(StructuralWork(descriptor_), {}, retired);
                }
                REQUIRE(created.size() == 2);
                REQUIRE(destroyed.empty());
                auto root = Object(1, 1);
                auto child = Object(1, 2);
                root.entity = created[0].entity;
                child.entity = created[1].entity;
                REQUIRE(created[1].parent == root.entity);
                const std::array added{root, child};
                return world_.PrepareObjectBatch(StructuralWork(descriptor_), added, {});
            }

        private:
            ReplicationWorldLifecycle &world_;
            ReplicationWorldActivationDescriptor descriptor_;
        };

        /** @brief Rejects a later owner after mapping preparation, so the Scene drops both candidates. */
        class RejectGroupParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Physics;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &,
                                                                               std::span<const Runtime::RuntimeEntityView>,
                                                                               std::span<const Runtime::EntityRef>) override {
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                    MakeError(NetworkErrors::ReplicationWorldInvalid));
            }
        };
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

    TEST_CASE("Authored object activation orders mappings and fences cancelled world replacements",
              "[unit][network][replication][lifecycle]") {
        auto lifecycle = ReplicationWorldLifecycle::Create({.maximumObjects = 2}).Value();
        const auto world = World();
        const auto replacement = World(11, 2, 2);
        const auto schema = Object().provenance.schema;
        const std::array objects{ReplicationAuthoredSceneObject{{world.scene, {7, 3}}, {schema, {1, 0}, Runtime::SceneObjectId{20}}},
                                 ReplicationAuthoredSceneObject{{world.scene, {8, 9}}, {schema, {1, 0}, Runtime::SceneObjectId{10}}}};
        REQUIRE(lifecycle.StageAuthoredScene(world, objects).HasValue());
        CHECK(lifecycle.Acquire(FixedWork(world)).HasError());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        const auto lease = lifecycle.Acquire(FixedWork(world)).Value();
        const auto rows = lease.Mapping().Entries();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].object.Slot() == 1);
        CHECK(rows[0].entity == objects[1].entity);
        CHECK(rows[1].object.Slot() == 2);
        CHECK(rows[1].entity == objects[0].entity);

        auto next = objects;
        for (auto &object : next)
            object.entity.runtime = replacement.scene;
        SECTION("duplicate provenance rolls back the detached candidate") {
            auto duplicate = next;
            duplicate[0].provenance.authoredObject = duplicate[1].provenance.authoredObject;
            RequireError(lifecycle.StageAuthoredScene(replacement, duplicate), NetworkErrors::NetworkObjectMappingConflict);
        }
        SECTION("stale Scene entities do not resolve against a new world") {
            RequireError(lifecycle.StageAuthoredScene(replacement, objects), NetworkErrors::NetworkObjectMappingInvalid);
        }
        SECTION("local client cannot assign authority-issued slots") {
            auto client = replacement;
            client.role = ReplicationExecutionRole::SimulatedClient;
            RequireError(lifecycle.StageAuthoredScene(client, next), NetworkErrors::ReplicationAuthorityDenied);
        }
        SECTION("cancellation after staging still prevents publication") {
            CancellationSource source;
            REQUIRE(lifecycle.StageAuthoredScene(replacement, next, source.Token()).HasValue());
            source.RequestCancellation();
            RequireError(lifecycle.CommitAtSafePoint(replacement.scene, replacement.session), NetworkErrors::ReplicationWorldCancelled);
        }
        CHECK_FALSE(lifecycle.HasStagedCandidate());
        CHECK_FALSE(lease.IsRevoked());
        CHECK(lifecycle.ActiveDescriptor().Value() == world);
        REQUIRE(lifecycle.StageAuthoredScene(replacement, next).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(replacement.scene, replacement.session).HasValue());
        CHECK(lease.IsRevoked());
        const auto current = lifecycle.Acquire(FixedWork(replacement)).Value();
        CHECK(current.Mapping().Entries().front().object.Epoch() == replacement.authority);
    }

    TEST_CASE("Replica activation matches the authority authored inventory to exact local Scene provenance",
              "[unit][network][replication][lifecycle]") {
        const auto authority = World(10, 2, 2);
        const auto client = World(20, 2, 2, ReplicationExecutionRole::AutonomousClient);
        auto server = ReplicationWorldLifecycle::Create({.maximumObjects = 4}).Value();
        auto replica = ReplicationWorldLifecycle::Create({.maximumObjects = 4}).Value();
        const auto schema = Object().provenance.schema;
        const std::array authored{ReplicationAuthoredSceneObject{{authority.scene, {9, 3}}, {schema, {1, 0}, Runtime::SceneObjectId{29}}},
                                  ReplicationAuthoredSceneObject{{authority.scene, {3, 8}}, {schema, {1, 0}, Runtime::SceneObjectId{12}}}};
        REQUIRE(server.StageAuthoredScene(authority, authored).HasValue());
        REQUIRE(server.CommitAtSafePoint(authority.scene, authority.session).HasValue());

        auto dynamic = Object(2, 3);
        dynamic.provenance.prefab =
            NetworkPrefabMemberProvenance{{{Assets::AssetId::FromBytes({5}), Sha256Digest{{1}}, 1}, 0}, dynamic.object};
        REQUIRE(server.RegisterObject(authority.scene, authority.session, dynamic).HasValue());
        const auto authorityLease = server.Acquire(FixedWork(authority)).Value();
        REQUIRE(authorityLease.Mapping().Entries().size() == 3);
        const auto captured = CaptureReplicationAuthoredScene(authorityLease).Value();
        NetworkAuthoredSceneAssembly assembly{captured.session, captured.authority};
        for (std::size_t index = 0; index < captured.objects.size(); ++index) {
            const auto page = MakeNetworkAuthoredScenePage(captured, index, 1).Value();
            const auto encoded = EncodeNetworkAuthoredScenePage(page).Value();
            REQUIRE(assembly.Append(DecodeNetworkAuthoredScenePage(encoded).Value()).HasValue());
        }
        REQUIRE(assembly.IsComplete());
        const auto complete = assembly.Take().Value();
        CHECK(complete.session == authority.session);
        CHECK(complete.authority == authority.authority);
        REQUIRE(complete.objects.size() == 2);
        CHECK(complete.objects[0].provenance.authoredObject == Runtime::SceneObjectId{12});
        CHECK(complete.objects[1].provenance.authoredObject == Runtime::SceneObjectId{29});
        const std::array local{ReplicationAuthoredSceneObject{{client.scene, {1, 5}}, authored[0].provenance},
                               ReplicationAuthoredSceneObject{{client.scene, {4, 2}}, authored[1].provenance}};

        SECTION("complete inventory is hidden until the structural safe point") {}
        SECTION("wrong Scene and incomplete inventory never produce a candidate") {
            RequireError(replica.StageReplicaAuthoredScene(client, authored, complete), NetworkErrors::NetworkObjectMappingInvalid);
            RequireError(replica.StageReplicaAuthoredScene(client, std::span{local}.first(1), complete),
                         NetworkErrors::ReplicationDescriptorIncompatible);
        }
        SECTION("exact session and epoch are required") {
            auto delayed = complete;
            delayed.session = Session(1);
            RequireError(replica.StageReplicaAuthoredScene(client, local, delayed), NetworkErrors::ReplicationWorldStale);
            delayed.session = client.session;
            delayed.authority = Authority(1);
            RequireError(replica.StageReplicaAuthoredScene(client, local, delayed), NetworkErrors::ReplicationWorldStale);
        }
        SECTION("permuted rows, aliases and schema drift are rejected") {
            auto incorrect = complete;
            std::swap(incorrect.objects[0], incorrect.objects[1]);
            RequireError(replica.StageReplicaAuthoredScene(client, local, incorrect), NetworkErrors::NetworkObjectMappingInvalid);
            incorrect = complete;
            incorrect.objects[1].provenance = incorrect.objects[0].provenance;
            RequireError(replica.StageReplicaAuthoredScene(client, local, incorrect), NetworkErrors::NetworkObjectMappingInvalid);
            incorrect = complete;
            incorrect.objects[1].provenance.schemaVersion = {1, 1};
            RequireError(replica.StageReplicaAuthoredScene(client, local, incorrect), NetworkErrors::ReplicationDescriptorIncompatible);
        }
        SECTION("cancellation cannot replace an active world") {
            const auto prior = World(19, 1, 1, ReplicationExecutionRole::AutonomousClient);
            REQUIRE(replica.Stage(prior).HasValue());
            REQUIRE(replica.CommitAtSafePoint(prior.scene, prior.session).HasValue());
            const auto published = replica.Acquire(FixedWork(prior)).Value();
            CancellationSource cancellation;
            REQUIRE(replica.StageReplicaAuthoredScene(client, local, complete, cancellation.Token()).HasValue());
            cancellation.RequestCancellation();
            RequireError(replica.CommitAtSafePoint(client.scene, client.session), NetworkErrors::ReplicationWorldCancelled);
            CHECK_FALSE(published.IsRevoked());
            CHECK(replica.ActiveDescriptor().Value() == prior);
        }
        SECTION("the full inventory must fit the configured world budget") {
            auto constrained = ReplicationWorldLifecycle::Create({.maximumObjects = 1}).Value();
            RequireError(constrained.StageReplicaAuthoredScene(client, local, complete), NetworkErrors::ReplicationWorldCapacityExceeded);
            CHECK_FALSE(constrained.HasStagedCandidate());
        }
        CHECK_FALSE(replica.HasStagedCandidate());
        REQUIRE(replica.StageReplicaAuthoredScene(client, local, complete).HasValue());
        CHECK(replica.Acquire(FixedWork(client)).HasError());
        REQUIRE(replica.CommitAtSafePoint(client.scene, client.session).HasValue());
        const auto published = replica.Acquire(FixedWork(client)).Value();
        const auto mappings = published.Mapping().Entries();
        REQUIRE(mappings.size() == 2);
        CHECK(mappings[0].object == complete.objects[0].object);
        CHECK(mappings[0].entity == local[1].entity);
        CHECK(mappings[1].object == complete.objects[1].object);
        CHECK(mappings[1].entity == local[0].entity);
        RequireError(CaptureReplicationAuthoredScene(published), NetworkErrors::ReplicationAuthorityDenied);
    }

    TEST_CASE("Network mappings and Scene parent-child groups publish and retire together", "[unit][network][replication][scene]") {
        using namespace Runtime;
        const auto world = World();
        auto lifecycle = ReplicationWorldLifecycle::Create({.maximumObjects = 2}).Value();
        REQUIRE(lifecycle.Stage(world).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        const ReplicationWorldWorkRequest captureWork{world.scene, world.session, RuntimePhase::NetworkFlush, 1};
        const auto initial = lifecycle.AcquireCaptureRead(captureWork).Value();

        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider bytes;
        JobSystem jobs{{1, 4}};
        Assets::AssetLoadService loads{jobs, bytes};
        RuntimeSceneService scenes{registry, loads, {}, world.scene};
        REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
        REQUIRE(scenes.AddStructuralParticipant(std::make_unique<GroupMappingParticipant>(lifecycle, world)).HasValue());
        bool reject{};
        SECTION("successful spawn and despawn share the same structural safe point") {}
        SECTION("failure of a later owner drops staged mappings and entities") {
            REQUIRE(scenes.AddStructuralParticipant(std::make_unique<RejectGroupParticipant>()).HasValue());
            reject = true;
        }
        const CancellationToken cancellation;
        REQUIRE(scenes.Startup(cancellation).HasValue());
        std::uint64_t frame{};
        const auto commit = [&] {
            REQUIRE(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = ++frame, .cancellation = cancellation})
                        .HasValue());
        };
        SceneDefinitionBuilder builder{{91}, {1}};
        REQUIRE(scenes.QueuePreparation(std::move(builder).Build().Value()).HasValue());
        commit();

        SceneCommandBuffer commands;
        const SceneStructuralAdmission admission{world.scene, registry.Snapshot().Revision(), {}};
        REQUIRE(commands.CreateGroup({{}, {.parentInGroup = 0}}, {}, admission).HasValue());
        const auto receipt = scenes.QueueTrackedStructuralCommands(std::move(commands)).Value();
        REQUIRE(initial.IsCurrent());
        REQUIRE(lifecycle.Acquire(FixedWork(world)).Value().Mapping().Entries().empty());
        commit();
        if (reject) {
            REQUIRE(receipt->Failure());
            CHECK_FALSE(receipt->ResultValue());
            CHECK(scenes.ActiveScene()->SlotCount() == 0);
            CHECK(initial.IsCurrent());
            CHECK(lifecycle.Acquire(FixedWork(world)).Value().Mapping().Entries().empty());
            return;
        }

        REQUIRE(receipt->ResultValue());
        const auto committed = lifecycle.Acquire(FixedWork(world)).Value();
        const auto rows = committed.Mapping().Entries();
        REQUIRE(rows.size() == 2);
        CHECK_FALSE(initial.IsCurrent());
        CHECK(scenes.ActiveScene()->Get(rows[1].entity).Value().parent == rows[0].entity);
        SceneCommandBuffer despawn;
        REQUIRE(despawn.DestroyGroup({rows[0].entity, rows[1].entity}, admission).HasValue());
        const auto removal = scenes.QueueTrackedStructuralCommands(std::move(despawn)).Value();
        commit();
        REQUIRE(removal->ResultValue());
        CHECK(scenes.ActiveScene()->Get(rows[0].entity).HasError());
        CHECK(scenes.ActiveScene()->Get(rows[1].entity).HasError());
        CHECK(lifecycle.Acquire(FixedWork(world)).Value().Mapping().Entries().empty());
    }

    TEST_CASE("A rejected object batch preserves the old mapping and capture revision", "[unit][network][replication][lifecycle]") {
        const auto world = World();
        auto lifecycle = ReplicationWorldLifecycle::Create({.maximumObjects = 2}).Value();
        REQUIRE(lifecycle.Stage(world).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        const auto old = Object();
        REQUIRE(lifecycle.RegisterObject(world.scene, world.session, old).HasValue());
        const auto pin = lifecycle.AcquireCaptureRead({world.scene, world.session, Runtime::RuntimePhase::NetworkFlush, 1}).Value();
        const std::array retired{old.object};
        const std::array replacement{Object(1, 1, 2, 10, 2)};
        SECTION("a late duplicate rolls back earlier changes in the same batch") {
            const std::array duplicate{replacement.front(), replacement.front()};
            RequireError(lifecycle.PrepareObjectBatch(StructuralWork(world), duplicate, retired),
                         NetworkErrors::NetworkObjectMappingConflict);
        }
        SECTION("cancellation after preparation rejects the pending transaction") {
            CancellationSource cancel;
            auto prepared = lifecycle.PrepareObjectBatch(StructuralWork(world, cancel.Token()), replacement, retired).Value();
            cancel.RequestCancellation();
            RequireError(prepared->ValidatePublication(), NetworkErrors::ReplicationWorldCancelled);
        }
        SECTION("paused worlds reject pending mapping publication") {
            auto prepared = lifecycle.PrepareObjectBatch(StructuralWork(world), replacement, retired).Value();
            REQUIRE(lifecycle.Pause(world.scene, world.session).HasValue());
            RequireError(prepared->ValidatePublication(), NetworkErrors::ReplicationWorldUnavailable);
            REQUIRE(lifecycle.Resume(world.scene, world.session).HasValue());
        }
        REQUIRE(pin.IsCurrent());
        CHECK(pin.Resolve(old.object).Value() == old);
        CHECK(pin.Resolve(replacement.front().object).HasError());
        auto prepared = lifecycle.PrepareObjectBatch(StructuralWork(world), replacement, retired).Value();
        const auto unrelated = Object(1, 2);
        REQUIRE(lifecycle.RegisterObject(world.scene, world.session, unrelated).HasValue());
        RequireError(prepared->ValidatePublication(), NetworkErrors::ReplicationWorldStale);
        auto pending = lifecycle.PrepareObjectBatch(StructuralWork(world), replacement, retired).Value();
        lifecycle.BeginShutdown();
        RequireError(pending->ValidatePublication(), NetworkErrors::ReplicationWorldStale);
    }

    TEST_CASE("Dynamic network identity issuance reuses retired generations without overwriting authored objects",
              "[unit][network][replication][lifecycle]") {
        const auto world = World();
        auto lifecycle = ReplicationWorldLifecycle::Create({.maximumObjects = 2}).Value();
        auto authored = Object();
        authored.provenance.authoredObject = Runtime::SceneObjectId{11};
        REQUIRE(lifecycle.StageAuthoredScene(world, std::array{ReplicationAuthoredSceneObject{authored.entity, authored.provenance}})
                    .HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        const auto schema = authored.provenance.schema;
        const auto version = authored.provenance.schemaVersion;
        const auto entity = Object(1, 2).entity;
        const auto nextEntity = Object(1, 2, 2, 10, 2).entity;
        const std::array original{ReplicationDynamicSceneObject{entity, schema, version}};
        const std::array next{ReplicationDynamicSceneObject{nextEntity, schema, version}};
        const auto work = StructuralWork(world);
        const auto publish = [](auto prepared) {
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value()->ValidatePublication().HasValue());
            prepared.Value()->Publish();
            REQUIRE(prepared.Value()->AfterPublication().HasValue());
        };
        RequireError(lifecycle.PrepareDynamicObjectBatch(work, original, std::array{authored.entity}),
                     NetworkErrors::NetworkObjectMappingInvalid);
        publish(lifecycle.PrepareDynamicObjectBatch(work, original, {}));
        const auto snapshot = [&] {
            return lifecycle.Acquire(FixedWork(world)).Value();
        };
        const auto first = snapshot();
        REQUIRE(first.Mapping().Entries().size() == 2);
        const auto firstDynamic = first.Mapping().Entries()[1];
        CHECK(firstDynamic.entity == entity);
        CHECK_FALSE(firstDynamic.provenance.authoredObject);

        const std::array retired{entity};
        const std::array duplicate{next.front(), next.front()};
        RequireError(lifecycle.PrepareDynamicObjectBatch(work, duplicate, retired), NetworkErrors::NetworkObjectMappingCapacityExceeded);
        CHECK(snapshot().Mapping().Entries()[1] == firstDynamic);
        publish(lifecycle.PrepareDynamicObjectBatch(work, next, retired));
        const auto replacement = snapshot();
        REQUIRE(replacement.Mapping().Entries().size() == 2);
        CHECK(replacement.Mapping().Entries()[0] == authored);
        CHECK(replacement.Mapping().Entries()[1].object == firstDynamic.object.NextGeneration().Value());
        CHECK(replacement.Mapping().Entries()[1].entity == nextEntity);
        RequireError(lifecycle.PrepareDynamicObjectBatch(work, {}, retired), NetworkErrors::NetworkObjectMappingUnknown);
    }

    TEST_CASE("Prefab mapping candidates reject inconsistent member evidence without publishing IDs",
              "[unit][network][replication][lifecycle][prefab]") {
        const auto world = World();
        auto lifecycle = ReplicationWorldLifecycle::Create({.maximumObjects = 4}).Value();
        REQUIRE(lifecycle.Stage(world).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        const auto work = StructuralWork(world);
        const auto first = Object(1, 1);
        const auto second = Object(1, 2);
        const NetworkPrefabTemplateIdentity recipe{Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                                                   Sha256Digest{{1}}, 3};
        std::array rows{ReplicationDynamicSceneObject{first.entity, first.provenance.schema, first.provenance.schemaVersion,
                                                      NetworkPrefabMemberSource{recipe, 0}},
                        ReplicationDynamicSceneObject{second.entity, second.provenance.schema, second.provenance.schemaVersion,
                                                      NetworkPrefabMemberSource{recipe, 2}}};
        const auto expected = rows.back().prefab;
        SECTION("duplicate prepared slot") {
            rows.back().prefab->entitySlot = 0;
        }
        SECTION("out-of-range prepared slot") {
            rows.back().prefab->entitySlot = recipe.entityCount;
        }
        SECTION("mixed recipe revisions") {
            rows.back().prefab->recipe.artifactDigest.bytes.front() = 2;
        }
        SECTION("mixed dynamic kinds") {
            rows.back().prefab.reset();
        }
        RequireError(lifecycle.PrepareDynamicObjectBatch(work, rows, {}), NetworkErrors::NetworkObjectMappingInvalid);
        CHECK(lifecycle.Acquire(FixedWork(world)).Value().Mapping().Entries().empty());
        rows.back().prefab = expected;
        auto prepared = lifecycle.PrepareDynamicObjectBatch(work, rows, {}).Value();
        REQUIRE(prepared->ValidatePublication().HasValue());
        prepared->Publish();
        REQUIRE(prepared->AfterPublication().HasValue());
        const auto lease = lifecycle.Acquire(FixedWork(world)).Value();
        const auto entries = lease.Mapping().Entries();
        REQUIRE(entries.size() == 2);
        REQUIRE(entries[0].provenance.prefab);
        REQUIRE(entries[1].provenance.prefab);
        CHECK(entries[0].object == first.object);
        CHECK(entries[1].object == second.object);
        CHECK(entries[0].provenance.prefab->occurrence == first.object);
        CHECK(entries[1].provenance.prefab->occurrence == first.object);
        CHECK(entries[1].provenance.prefab->source == *expected);
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

    TEST_CASE("Replication mapping metrics count only committed exact-generation changes", "[unit][network][replication][metrics]") {
        NetworkMetrics metrics{88, true};
        auto lifecycle = std::move(ReplicationWorldLifecycle::Create({.maximumObjects = 2, .maximumRetiredWorlds = 2}, &metrics)).Value();
        const auto world = World();
        REQUIRE(lifecycle.Stage(world).HasValue());
        REQUIRE(lifecycle.CommitAtSafePoint(world.scene, world.session).HasValue());
        const auto object = Object();
        REQUIRE(lifecycle.RegisterObject(world.scene, world.session, object).HasValue());
        RequireError(lifecycle.RegisterObject(world.scene, world.session, object), NetworkErrors::NetworkObjectMappingConflict);
        REQUIRE(lifecycle.RetireObject(world.scene, world.session, object.object).HasValue());
        RequireError(lifecycle.RetireObject(world.scene, world.session, object.object), NetworkErrors::NetworkObjectMappingUnknown);
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.replication[static_cast<std::size_t>(NetworkMetricReplication::ObjectRegistered)] == 1);
        REQUIRE(snapshot.replication[static_cast<std::size_t>(NetworkMetricReplication::ObjectRetired)] == 1);
        lifecycle.BeginShutdown();
        REQUIRE(metrics.Close());
    }
}  // namespace Horo::Network
