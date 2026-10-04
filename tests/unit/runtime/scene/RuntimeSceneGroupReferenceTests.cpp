#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        /** @brief Scene-reference unit observer only; not native Physics or full prefab acceptance evidence. */
        class ReferenceObserver final : public SceneStructuralParticipant {
            class Candidate final : public SceneStructuralCandidate {
            public:
                Result<void> ValidatePublication() const override {
                    return Result<void>::Success();
                }

                void Publish() noexcept override {}

                Result<void> AfterPublication() override {
                    return Result<void>::Success();
                }
            };

        public:
            SceneStructuralOwner Owner() const noexcept override {
                return SceneStructuralOwner::Physics;
            }

            Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(RuntimeSceneView, std::span<const RuntimeEntityView>,
                                                                      std::span<const EntityRef>) override {
                return Result<std::unique_ptr<SceneStructuralCandidate>>::Success(std::make_unique<Candidate>());
            }
        };

        RuntimeEntityCreateInfo Body() {
            RuntimeEntityCreateInfo info;
            info.components.rigidBody = RigidBodyComponent{.id = {11}, .body = {1}};
            return info;
        }

        RuntimeEntityGroupEntry Reference(std::variant<RuntimeGroupEntitySlot, EntityRef> target) {
            RuntimeEntityGroupEntry entry;
            PhysicsConstraintComponent constraint;
            constraint.id = {12};
            constraint.constraint = {1};
            constraint.first.body = {{1}, {1}};
            entry.info.components.physicsConstraints.push_back(constraint);
            entry.physicsReferences.push_back({GroupPhysicsReferenceKind::ConstraintFirst, 0, target, {1}});
            return entry;
        }

        struct Fixture {
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider bytes;
            JobSystem jobs{{1, 4}};
            Assets::AssetLoadService loads{jobs, bytes};
            RuntimeSceneService scenes{registry, loads};
            CancellationSource cancellation;
            EntityRef external;

            Fixture() {
                REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
                REQUIRE(scenes.AddStructuralParticipant(std::make_unique<ReferenceObserver>()).HasValue());
                REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
                SceneDefinitionBuilder builder{{91}, {1}};
                auto definition = std::move(builder).Build();
                REQUIRE(definition.HasValue());
                REQUIRE(scenes.QueuePreparation(std::move(definition).Value()).HasValue());
                Commit();
                SceneCommandBuffer seed;
                (void)seed.Create(Body());
                REQUIRE(scenes.QueueStructuralCommands(std::move(seed)).HasValue());
                Commit();
                auto result = scenes.TakeStructuralCommitResult();
                REQUIRE(result);
                external = result->created.front().entity;
            }

            void Commit() {
                REQUIRE(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {1, {}, 0.0, 0, {}, false, cancellation.Token()})
                            .HasValue());
            }

            void AddGroup(SceneCommandBuffer &commands, std::vector<RuntimeEntityGroupEntry> entries) {
                REQUIRE(commands
                            .CreateGroup(std::move(entries), {},
                                         {scenes.ActiveScene()->RuntimeId(), registry.Snapshot().Revision(), cancellation.Token(), {}})
                            .HasValue());
            }
        };

        TEST_CASE("Scene group external body references require previously committed live authority",
                  "[runtime][scene][group][references]") {
            Fixture fixture;
            EntityRef target = fixture.external;
            SceneCommandBuffer commands;
            bool destroyAfterGroup{};
            SECTION("foreign scene") {
                ++target.runtime.value;
            }
            SECTION("stale generation") {
                ++target.entity.generation;
            }
            SECTION("scheduled destruction") {
                commands.Destroy(target);
            }
            SECTION("scheduled destruction after the group") {
                destroyAfterGroup = true;
            }
            SECTION("candidate-only guessed slot") {
                (void)commands.Create(Body());
                target.entity.index = 1;
            }
            fixture.AddGroup(commands, {Reference(target)});
            if (destroyAfterGroup)
                commands.Destroy(target);
            REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(commands)).HasValue());
            fixture.Commit();
            REQUIRE(fixture.scenes.TakeOperationError());
            REQUIRE_FALSE(fixture.scenes.TakeStructuralCommitResult());
            REQUIRE(fixture.scenes.ActiveScene()->Get(fixture.external).HasValue());
            REQUIRE(fixture.scenes.ActiveScene()->SlotCount() == 1);
        }

        TEST_CASE("Scene group external body fixups retain exact committed authority", "[runtime][scene][group][references]") {
            Fixture fixture;
            SceneCommandBuffer commands;
            fixture.AddGroup(commands, {Reference(fixture.external)});
            REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(commands)).HasValue());
            fixture.Commit();
            REQUIRE_FALSE(fixture.scenes.TakeOperationError());
            const auto result = fixture.scenes.TakeStructuralCommitResult();
            REQUIRE(result);
            REQUIRE(result->created.size() == 1);
            const auto view = fixture.scenes.ActiveScene()->Get(result->created.front().entity);
            REQUIRE(view.HasValue());
            REQUIRE(view.Value().physicsReferences.size() == 1);
            REQUIRE(view.Value().physicsReferences.front().target == fixture.external);
            REQUIRE(fixture.scenes.ActiveScene()->Get(fixture.external).HasValue());
        }

        TEST_CASE("Scene group local body fixups allow forward slots and clone exact runtime generations",
                  "[runtime][scene][group][references]") {
            Fixture fixture;
            SceneCommandBuffer commands;
            RuntimeEntityGroupEntry body;
            body.info = Body();
            fixture.AddGroup(commands, {Reference(RuntimeGroupEntitySlot{1}), body});
            REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(commands)).HasValue());
            fixture.Commit();
            REQUIRE_FALSE(fixture.scenes.TakeOperationError());
            auto result = fixture.scenes.TakeStructuralCommitResult();
            REQUIRE(result);
            REQUIRE(result->created.size() == 2);
            const auto source = result->created[0].entity;
            const auto target = result->created[1].entity;
            const auto view = fixture.scenes.ActiveScene()->Get(source);
            REQUIRE(view.HasValue());
            REQUIRE(view.Value().physicsReferences.size() == 1);
            REQUIRE(view.Value().physicsReferences.front().target == target);
            REQUIRE_FALSE(view.Value().authoredObject);
            auto clone = fixture.scenes.CloneActive({999});
            REQUIRE(clone.HasValue());
            const auto cloned = clone.Value()->View().Get({{999}, source.entity});
            REQUIRE(cloned.HasValue());
            REQUIRE((cloned.Value().physicsReferences.front().target == EntityRef{{999}, target.entity}));
            REQUIRE(view.Value().physicsReferences.front().target == target);
        }
    }  // namespace
}  // namespace Horo::Runtime
