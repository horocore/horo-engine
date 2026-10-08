#pragma once

/** @file PerceptionSightTestSupport.h
 * @brief Target-private fixtures for sight geometry and canonical batch/lifecycle regressions.
 */

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSight.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <utility>

namespace Horo::AI {
    /** @brief Fault injection into copied completion values only; no private Physics/native state is accessed. */
    struct PerceptionSightTestAccess final {
        [[nodiscard]] static Physics::PhysicsQueryBatchCompletion Completion(const PerceptionSight &sight) {
            REQUIRE(sight.batch_.has_value());
            const auto completed = sight.batch_->Poll();
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
            return *completed.Value();
        }

        [[nodiscard]] static Result<PerceptionSightResult> Consume(PerceptionSight &sight,
                                                                   const Physics::PhysicsQueryBatchCompletion &completion) {
            return sight.Consume(completion);
        }

        [[nodiscard]] static bool Pending(const PerceptionSight &sight) {
            return sight.batch_.has_value() || sight.result_.pending;
        }

        [[nodiscard]] static PerceptionSightResult Current(const PerceptionSight &sight) {
            return sight.result_;
        }
    };

    namespace SightTest {
        /** @brief Checks fixture construction before transferring ownership, preserving operation and typed failure context. */
        template <typename Value> [[nodiscard]] Value RequireFixtureValue(Result<Value> result, const std::string_view operation) {
            INFO(operation);
            INFO("error code: " << (result.HasError() ? result.ErrorValue().code.Value() : ""));
            INFO("error message: " << (result.HasError() ? result.ErrorValue().message : ""));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct SightScene final {
            std::unique_ptr<Runtime::RuntimeScene> scene;
            Runtime::EntityRef listener;
            Runtime::EntityRef target;
            Runtime::EntityRef secondTarget;
            PerceptionSpatialBroadphase broadphase;
            std::shared_ptr<const PerceptionSpatialSnapshot> spatial;
            std::uint64_t revision{};

            explicit SightScene(const std::uint64_t incarnation = 77) {
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
                for (std::uint64_t object = 1; object <= 3; ++object)
                    builder.Add({.object = Runtime::SceneObjectId{object}});
                const auto definition = RequireFixtureValue(std::move(builder).Build(), "build sight fixture Scene definition");
                scene = RequireFixtureValue(Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{incarnation}),
                                            "create sight fixture runtime Scene");
                listener = *scene->View().Find(Runtime::SceneObjectId{1});
                target = *scene->View().Find(Runtime::SceneObjectId{2});
                secondTarget = *scene->View().Find(Runtime::SceneObjectId{3});
                Publish();
            }

            void Publish(const Math::WorldCoordinate64 &position = Math::WorldCoordinate64::FromMillimeters(0, 0, -10'000),
                         const Math::WorldCoordinate64 &listenerPosition = {}, const bool second = false) {
                PerceptionSpatialListener declared{.entity = listener, .position = listenerPosition};
                declared.senses[0] = SenseTypeIds::Sight;
                declared.senseCount = 1;
                std::array<PerceptionSpatialSource, 2> sources{};
                sources[0] = {.entity = target, .position = position, .layers = 1};
                sources[0].senses[0] = SenseTypeIds::Sight;
                sources[0].senseCount = 1;
                sources[1] = sources[0];
                sources[1].entity = secondTarget;
                spatial = RequireFixtureValue(broadphase.Publish(scene->View(), ++revision, std::span{&declared, 1},
                                                                 std::span{sources.data(), second ? 2U : 1U}),
                                              "publish sight fixture candidates");
            }
        };

        [[nodiscard]] inline Physics::PhysicsQueryChannelId VisibilityChannel() {
            return Physics::PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        }

        [[nodiscard]] inline PerceptionSightFrame Frame(const Runtime::EntityRef listener) {
            return {.listener = listener,
                    .physicsSceneGeneration = 7,
                    .physicsPublicationRevision = 1,
                    .physicsCompletedTick = 1,
                    .occlusionFilter = {.channel = VisibilityChannel()}};
        }

        [[nodiscard]] inline PerceptionSightPhysicsPublication Publication(const Physics::PhysicsQueryEventCapability &capability,
                                                                           const PerceptionSightFrame &frame) {
            return {.identity = capability.Identity(),
                    .sceneGeneration = frame.physicsSceneGeneration,
                    .publicationRevision = frame.physicsPublicationRevision,
                    .completedTick = frame.physicsCompletedTick};
        }

#if HORO_TEST_PHYSICS_NATIVE
        struct SightWorld final {
            std::unique_ptr<Physics::PhysicsRuntime> runtime;
            std::unique_ptr<Physics::PhysicsWorld> world;
            Physics::PhysicsQueryEventCapability capability;
            std::uint64_t tick{};

            SightWorld() {
                runtime = RequireFixtureValue(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical),
                                              "create canonical sight fixture Physics runtime");
                Physics::PhysicsWorldSettingsDescriptor settings;
                settings.world.capacity = {64, 128, 16, 4096};
                settings.budgets.scratchBytes = 1024 * 1024;
                const auto captured =
                    RequireFixtureValue(Physics::PhysicsWorldSettings::Capture(settings), "capture sight fixture Physics settings");
                world = RequireFixtureValue(runtime->PrepareWorld(captured), "prepare sight fixture Physics world");
                REQUIRE(world->Activate(Physics::PhysicsWorldId::Create(51).Value()).HasValue());
                capability = RequireFixtureValue(world->IssueQueryEventCapability(), "issue sight fixture Physics query capability");
                Advance();
            }

            void Advance() {
                REQUIRE(world
                            ->AdvanceFixedTick(
                                {.simulationTick = ++tick, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
            }

            [[nodiscard]] PerceptionSightFrame CurrentFrame(const Runtime::EntityRef listener) const {
                auto frame = Frame(listener);
                frame.physicsPublicationRevision = world->PublishedTick().publicationRevision;
                frame.physicsCompletedTick = world->PublishedTick().completedTick;
                return frame;
            }

            [[nodiscard]] Physics::PhysicsQueryFixture Box(
                const float z, const Physics::PhysicsQueryFixtureResponse response = Physics::PhysicsQueryFixtureResponse::Block) const {
                return world
                    ->CreateQueryFixture({.shape = Physics::PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                          .pose = {.translation = {0, 0, z}, .rotation = Math::Quaternion::Identity()},
                                          .layer = Physics::CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                                          .profile = Physics::CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value(),
                                          .channel = VisibilityChannel(),
                                          .response = response})
                    .Value();
            }

            [[nodiscard]] PerceptionSightFrame Queue(PerceptionSight &sight, const SightScene &scene,
                                                     const PerceptionSightPolicy &policy = {}, const std::size_t rays = 128) const {
                const auto frame = CurrentFrame(scene.listener);
                PerceptionSightBudget budget{.simulationTick = tick, .remainingRaycasts = rays};
                REQUIRE(sight.Begin(scene.scene->View(), *scene.spatial, capability, frame, policy, budget).HasValue());
                return frame;
            }

            [[nodiscard]] PerceptionSightFrame CompletePending(PerceptionSight &sight, const SightScene &scene) const {
                const auto frame = Queue(sight, scene, {}, 1);
                REQUIRE(world->ProcessQueryBatch().HasValue());
                return frame;
            }

            [[nodiscard]] PerceptionSightResult Sample(PerceptionSight &sight, const SightScene &scene,
                                                       const PerceptionSightPolicy &policy = {}, const std::size_t rays = 128) const {
                const auto frame = Queue(sight, scene, policy, rays);
                REQUIRE(world->ProcessQueryBatch().HasValue());
                auto completed = sight.Poll(scene.scene->View(), scene.revision, Publication(capability, frame));
                REQUIRE(completed.HasValue());
                REQUIRE_FALSE(completed.Value().pending);
                return completed.Value();
            }
        };

#endif
    }  // namespace SightTest
}  // namespace Horo::AI
