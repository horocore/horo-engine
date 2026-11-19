#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"
#include "PrefabAtomicOwnerTestSupport.h"

namespace Horo::Application::PrefabAtomicTest {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
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

    }  // namespace
#endif
}  // namespace Horo::Application::PrefabAtomicTest
