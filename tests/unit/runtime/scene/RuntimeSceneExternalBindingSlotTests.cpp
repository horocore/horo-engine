#include "GameplayStructuralUnitObserver.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        Gameplay::ComponentTypeId Health() {
            return Gameplay::ComponentTypeId::Parse("game.tests.health").Value();
        }

        /** @brief Complete typed Scene command projection; native registry/provider qualification belongs to prefab integration tests. */
        RuntimeEntityGroupEntry BindingEntry(std::uint64_t behaviorId, std::size_t target,
                                             std::optional<Gameplay::ComponentTypeId> expected, bool withComponent = true) {
            RuntimeEntityGroupEntry entry;
            const auto type = Gameplay::BehaviorTypeId::Parse("game.tests.binding_owner").Value();
            entry.info.components.behaviors.emplace_back(Gameplay::BehaviorInstanceId{behaviorId}, type);
            entry.members.push_back({type, behaviorId});
            if (withComponent) {
                Gameplay::SerializedComponent component;
                component.typeId = Health();
                component.payload = {std::byte{'{'}, std::byte{'}'}};
                entry.info.components.gameplayComponents.push_back(std::move(component));
                entry.members.push_back({Health(), 7});
            }
            entry.references.push_back({0, 11, RuntimeGroupExternalEntitySlot{{target}, std::move(expected)}});
            return entry;
        }

        struct BindingFixture final {
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider bytes;
            JobSystem jobs{{1, 4}};
            Assets::AssetLoadService loads{jobs, bytes};
            RuntimeSceneService scenes{registry, loads};
            std::shared_ptr<Tests::GameplayStructuralUnitEvents> structural{std::make_shared<Tests::GameplayStructuralUnitEvents>()};
            CancellationSource cancellation;
            CancellationToken token{cancellation.Token()};

            explicit BindingFixture(bool composeGameplay = true) {
                if (composeGameplay)
                    REQUIRE(
                        scenes.AddStructuralParticipant(std::make_unique<Tests::GameplayStructuralUnitObserver>(structural)).HasValue());
                REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
                REQUIRE(scenes.Startup(token).HasValue());
                SceneDefinitionBuilder builder{{91}, {1}};
                REQUIRE(scenes.QueuePreparation(std::move(builder).Build().Value()).HasValue());
                Commit();
            }

            void Commit() {
                REQUIRE(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 1, .cancellation = token}).HasValue());
            }

            void Queue(std::vector<RuntimeEntityGroupEntry> entries) {
                SceneCommandBuffer commands;
                REQUIRE(
                    commands.CreateGroup(std::move(entries), {}, {scenes.ActiveScene()->RuntimeId(), registry.Snapshot().Revision(), token})
                        .HasValue());
                REQUIRE(scenes.QueueStructuralCommands(std::move(commands)).HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Aggregate-local external entity slots preserve their category for forward and cyclic typed bindings",
              "[runtime][scene][group][external-binding]") {
        BindingFixture fixture;
        std::optional<Gameplay::ComponentTypeId> expected;
        SECTION("entity relationship") {}
        SECTION("component relationship") {
            expected = Health();
        }
        fixture.Queue({BindingEntry(1, 1, expected), BindingEntry(2, 0, expected)});
        fixture.Commit();
        REQUIRE_FALSE(fixture.scenes.TakeOperationError());
        CHECK(fixture.structural->prepared == 1);
        CHECK(fixture.structural->published == 1);
        const auto receipt = fixture.scenes.TakeStructuralCommitResult();
        REQUIRE(receipt);
        REQUIRE(receipt->created.size() == 2);
        const auto scene = *fixture.scenes.ActiveScene();
        const auto first = scene.Get(receipt->created[0].entity).Value();
        const auto second = scene.Get(receipt->created[1].entity).Value();
        REQUIRE(first.groupReferences.size() == 1);
        REQUIRE(second.groupReferences.size() == 1);
        REQUIRE(std::holds_alternative<RuntimeGroupExternalReference>(first.groupReferences[0].target));
        REQUIRE(std::holds_alternative<RuntimeGroupExternalReference>(second.groupReferences[0].target));
        const auto &forward = std::get<RuntimeGroupExternalReference>(first.groupReferences[0].target);
        const auto &backward = std::get<RuntimeGroupExternalReference>(second.groupReferences[0].target);
        CHECK(forward.entity == second.entity);
        CHECK(backward.entity == first.entity);
        CHECK(forward.component == expected);
        CHECK(backward.component == expected);
    }

    TEST_CASE("Aggregate-local external slots reject absent incompatible or out-of-range targets with whole group rollback",
              "[runtime][scene][group][external-binding][rollback]") {
        BindingFixture fixture;
        auto entries = std::vector{BindingEntry(1, 1, Health()), BindingEntry(2, 0, {})};
        SECTION("out-of-range dense slot") {
            entries[0].references[0].target = RuntimeGroupExternalEntitySlot{{2}, Health()};
        }
        SECTION("missing required component") {
            entries[1] = BindingEntry(2, 0, {}, false);
        }
        SECTION("incompatible actual target type") {
            entries[0].references[0].target =
                RuntimeGroupExternalEntitySlot{{1}, Gameplay::ComponentTypeId::Parse("game.tests.other").Value()};
        }
        const auto before = fixture.scenes.ActiveScene()->StructuralRevision();
        fixture.Queue(std::move(entries));
        fixture.Commit();
        REQUIRE(fixture.scenes.TakeOperationError());
        CHECK_FALSE(fixture.scenes.TakeStructuralCommitResult());
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
        CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == before);
    }
}  // namespace Horo::Runtime

namespace Horo::Runtime {
    TEST_CASE("Ordinary public group admission rejects a forged private restore partition token",
              "[runtime][scene][group][partitions][admission]") {
        BindingFixture fixture;
        auto entry = BindingEntry(1, 0, {});
        entry.restorePartition = 0;
        SceneCommandBuffer commands;
        CHECK(commands
                  .CreateGroup({entry}, {},
                               {fixture.scenes.ActiveScene()->RuntimeId(), fixture.registry.Snapshot().Revision(), fixture.token})
                  .HasError());
        CHECK(commands.Empty());
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
    }

    TEST_CASE("External slot unit fixture still rejects omitted actual Gameplay structural owner", "[runtime][scene][group][owner]") {
        BindingFixture fixture{false};
        fixture.Queue({BindingEntry(1, 1, {}), BindingEntry(2, 0, {})});
        fixture.Commit();
        REQUIRE(fixture.scenes.TakeOperationError());
        CHECK_FALSE(fixture.scenes.TakeStructuralCommitResult());
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
    }
}  // namespace Horo::Runtime
