#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkPrefabLifecycle.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"
#include "PrefabAtomicOwnerTestSupport.h"

namespace Horo::Application::PrefabAtomicTest {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Maps exact authority objects through the client Scene publication callback. */
        class ReplicaGroupPublication final : public Runtime::SceneGroupPublication {
        public:
            ReplicaGroupPublication(Network::ReplicationWorldLifecycle &world, Network::NetworkPrefabLifecycleRecord spawn)
                : world_(world), spawn_(std::move(spawn)) {}

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Runtime::EntityRef> members,
                                                                               bool creating) const override {
                const Network::ReplicationWorldWorkRequest work{.scene = scene.RuntimeId(),
                                                                .session = spawn_.session,
                                                                .phase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges};
                return world_.PrepareReplicaPrefab(work, spawn_, members, creating);
            }

        private:
            Network::ReplicationWorldLifecycle &world_;
            Network::NetworkPrefabLifecycleRecord spawn_;
        };

        /** @brief Issues network identities for Scene handles in the verified cooked recipe order. */
        class NetworkGroupPublication final : public Runtime::SceneGroupPublication {
        public:
            NetworkGroupPublication(Network::ReplicationWorldLifecycle &world, Network::ReplicationWorldActivationDescriptor descriptor)
                : world_(world), descriptor_(descriptor) {}

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Runtime::EntityRef> members,
                                                                               bool creating) const override {
                REQUIRE(scene.RuntimeId() == descriptor_.scene);
                REQUIRE(members.size() == 2);
                const Network::ReplicationWorldWorkRequest work{descriptor_.scene,
                                                                descriptor_.session,
                                                                Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                                                1,
                                                                {}};
                if (!creating)
                    return world_.PrepareDynamicObjectBatch(work, {}, members);
                const auto schema = Network::ReplicationSchemaId::Create(7).Value();
                const std::array added{Network::ReplicationDynamicSceneObject{members[0], schema, {1, 0}},
                                       Network::ReplicationDynamicSceneObject{members[1], schema, {1, 0}}};
                return world_.PrepareDynamicObjectBatch(work, added, {});
            }

        private:
            Network::ReplicationWorldLifecycle &world_;
            Network::ReplicationWorldActivationDescriptor descriptor_;
        };

        /** @brief Test host policy opting in the two-entity cooked occurrence as a whole. */
        class NetworkGroupOwner final : public Prefab::IPrefabGroupPublicationOwner {
        public:
            explicit NetworkGroupOwner(Network::ReplicationWorldLifecycle &world) : world_(world) {}

            Result<std::shared_ptr<const Runtime::SceneGroupPublication>> Bind(const Prefab::PreparedPrefabSpawn &recipe,
                                                                               const Prefab::PrefabSpawnRequest &request,
                                                                               std::span<const Runtime::RuntimeComponentSet> projected,
                                                                               std::string_view creator) const override {
                REQUIRE(creator == "game.tests");
                REQUIRE(recipe.Data().assetId == request.source.asset);
                REQUIRE(recipe.Data().entities.size() == 2);
                REQUIRE(projected.size() == recipe.Data().entities.size());
                REQUIRE_FALSE(recipe.Data().entities[0].parent);
                REQUIRE(recipe.Data().entities[1].parent == Prefab::CookedPrefabEntitySlot{0});
                const auto descriptor = world_.ActiveDescriptor();
                REQUIRE(descriptor.HasValue());
                return Result<std::shared_ptr<const Runtime::SceneGroupPublication>>::Success(
                    std::make_shared<const NetworkGroupPublication>(world_, descriptor.Value()));
            }

        private:
            Network::ReplicationWorldLifecycle &world_;
        };

        TEST_CASE("Production cooked Prefab groups publish and retire exact network mappings with Scene",
                  "[prefab][integration][atomic][network]") {
            bool reject = false;
            SECTION("A capacity rejection leaves every Scene owner unchanged") {
                reject = true;
            }
            SECTION("Both members map and retire in one publication") {}
            auto world = Network::ReplicationWorldLifecycle::Create(
                             {.maximumObjects = reject ? std::size_t{1} : std::size_t{4}, .maximumRetiredWorlds = 1})
                             .Value();
            const auto authority = Network::ReplicationAuthorityEpoch::Create(1).Value();
            const std::array objects{Network::NetworkObjectId::Create(authority, 1, 1).Value(),
                                     Network::NetworkObjectId::Create(authority, 2, 1).Value()};
            Fixture fixture{false, Fault::None, {}, {}, {}, nullptr, {}, {}, std::make_shared<NetworkGroupOwner>(world)};
            const auto baseline = *fixture.scenes.ActiveScene();
            const auto baselineRevision = baseline.StructuralRevision();
            using enum Runtime::RuntimePhase;
            const Network::ReplicationWorldActivationDescriptor descriptor{baseline.RuntimeId(),
                                                                           Network::NetworkSessionGeneration::Create(1).Value(), authority,
                                                                           Network::ReplicationExecutionRole::AuthorityServer,
                                                                           Network::ReplicationWorldPhaseSet::From(
                                                                               {NetworkPoll, FixedUpdate, NetworkFlush,
                                                                                CommitDeferredLifecycleChanges})};
            REQUIRE(world.Stage(descriptor).HasValue());
            REQUIRE(world.CommitAtSafePoint(descriptor.scene, descriptor.session).HasValue());
            const Network::ReplicationWorldWorkRequest capture{descriptor.scene, descriptor.session, NetworkFlush, 1, {}};
            const auto empty = world.AcquireCaptureRead(capture).Value();
            fixture.Tick(1);
            if (reject) {
                const auto operation = fixture.AwaitRejection();
                REQUIRE(operation.Failure() != nullptr);
                CHECK(operation.Failure()->code.Value() == Network::NetworkErrors::ReplicationWorldCapacityExceeded.code.Value());
                CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == baselineRevision);
                CHECK(fixture.Count() == 1);
                CHECK(fixture.Bodies() == 0);
                CHECK(fixture.ai.Snapshot().Value().Agents().size() == 1);
                CHECK(empty.IsCurrent());
                CHECK(empty.Resolve(objects[0]).HasError());
                CHECK(empty.Resolve(objects[1]).HasError());
                return;
            }

            fixture.Reach(3);
            CHECK_FALSE(empty.IsCurrent());
            const auto beforeRetirement = world.AcquireCaptureRead(capture).Value();
            const auto root = beforeRetirement.Resolve(objects[0]).Value().entity;
            const auto child = beforeRetirement.Resolve(objects[1]).Value().entity;
            const auto scene = *fixture.scenes.ActiveScene();
            CHECK(root == fixture.SpawnOperation().Spawned().Value().Root());
            CHECK(scene.Get(child).Value().parent == root);
            CHECK(fixture.Count() == 3);
            CHECK(fixture.Bodies() == 1);
            CHECK(fixture.ai.Snapshot().Value().Agents().size() == 3);

            fixture.Tick(2);
            fixture.Reach(1);
            CHECK_FALSE(beforeRetirement.IsCurrent());
            const auto retired = world.AcquireCaptureRead(capture).Value();
            CHECK(retired.Resolve(objects[0]).HasError());
            CHECK(retired.Resolve(objects[1]).HasError());
            CHECK(fixture.scenes.ActiveScene()->Get(root).HasError());
            CHECK(fixture.scenes.ActiveScene()->Get(child).HasError());
            CHECK(fixture.Count() == 1);
            CHECK(fixture.Bodies() == 0);
            CHECK(fixture.ai.Snapshot().Value().Agents().size() == 1);
        }

        TEST_CASE("Cooked replica spawn and despawn publish exact authority mappings without a gameplay request",
                  "[prefab][integration][atomic][network]") {
            auto world = Network::ReplicationWorldLifecycle::Create().Value();
            Fixture fixture;
            const auto scene = fixture.scenes.ActiveScene()->RuntimeId();
            const auto epoch = Network::ReplicationAuthorityEpoch::Create(17).Value();
            const auto session = Network::NetworkSessionGeneration::Create(6).Value();
            const Network::ReplicationWorldActivationDescriptor descriptor{scene, session, epoch,
                                                                           Network::ReplicationExecutionRole::AutonomousClient};
            REQUIRE(world.Stage(descriptor).HasValue());
            REQUIRE(world.CommitAtSafePoint(scene, session).HasValue());
            const auto schema = Network::ReplicationSchemaId::Create(7).Value();
            const auto submit = [&](std::uint32_t generation, Assets::AssetId asset, const CancellationToken &cancellation) {
                const auto object = Network::NetworkObjectId::Create(epoch, 5, generation).Value();
                const Network::NetworkPrefabLifecycleRecord record{session, object,
                                                                   Network::NetworkPrefabSpawnRecord{{asset, fixture.artifact, 2},
                                                                                                     {{1, object, schema, {1, 0}}}}};
                return fixture.gameplay->SpawnReplicaPrefab({.source = {asset, fixture.artifact, fixture.target}},
                                                            std::make_shared<const ReplicaGroupPublication>(world, record), cancellation);
            };
            const auto mapped = [&] {
                return world
                    .AcquireFor({scene, session, Runtime::RuntimePhase::NetworkPoll},
                                Network::ReplicationWorldCapability::ApplyAuthoritativeState)
                    .Value();
            };
            using enum Prefab::PrefabSpawnState;
            const auto sceneCancellation = fixture.cancellation.Token();
            const auto unknown = submit(3, Assets::AssetId::FromBytes({9}), sceneCancellation);
            REQUIRE(unknown.HasError());
            CHECK(unknown.ErrorValue().code.Value() == Prefab::PrefabErrors::AssetNotFound.code.Value());
            CancellationSource cancel;
            const auto cancelled = submit(3, fixture.asset, cancel.Token()).Value();
            cancel.RequestCancellation();
            fixture.AwaitState(cancelled, Cancelled);
            CHECK(fixture.Count() == 1);
            const auto created = submit(3, fixture.asset, sceneCancellation).Value();
            CHECK(mapped().Mapping().Entries().empty());
            fixture.AwaitState(created, Committed);
            const auto instance = created.Spawned().Value();
            const auto active = mapped();
            const auto records = active.Mapping().Entries();
            REQUIRE(records.size() == 1);
            CHECK(records.front().object.Generation() == 3);
            CHECK(fixture.scenes.ActiveScene()->Get(records.front().entity).Value().parent == instance.Root());

            const auto duplicate = submit(3, fixture.asset, sceneCancellation).Value();
            fixture.AwaitState(duplicate, Failed);
            REQUIRE(duplicate.Failure());
            CHECK(duplicate.Failure()->code.Value() == Network::NetworkErrors::NetworkObjectMappingConflict.code.Value());
            CHECK(fixture.Count() == 3);
            const auto removal = fixture.gameplay->DespawnReplicaPrefab(instance, fixture.cancellation.Token()).Value();
            fixture.AwaitState(removal, Committed);
            CHECK(mapped().Mapping().Entries().empty());
            CHECK(fixture.Count() == 1);
            const auto stale = submit(3, fixture.asset, sceneCancellation).Value();
            fixture.AwaitState(stale, Failed);
            REQUIRE(stale.Failure());
            CHECK(stale.Failure()->code.Value() == Network::NetworkErrors::NetworkObjectMappingUnknown.code.Value());
            CHECK(fixture.Count() == 1);

            const auto replacement = submit(6, fixture.asset, sceneCancellation).Value();
            fixture.AwaitState(replacement, Committed);
            const auto staleRemoval = fixture.gameplay->DespawnReplicaPrefab(instance, sceneCancellation);
            if (staleRemoval.HasValue())
                fixture.AwaitState(staleRemoval.Value(), Failed);
            const auto next = mapped();
            REQUIRE(next.Mapping().Entries().size() == 1);
            CHECK(next.Mapping().Entries().front().object.Generation() == 6);
        }

    }  // namespace
#endif
}  // namespace Horo::Application::PrefabAtomicTest
