#include "CharacterReferenceSceneCorpus.h"
#include "CharacterReferenceSceneCorpusTestHelpers.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace Corpus;

        TEST_CASE("Character reference manifest has canonical stable identities and finite schedules", "[physics][character-reference]") {
            CHECK(Version == 1);
            REQUIRE(Recipes.size() == 15);
            std::array<bool, Recipes.size()> kinds{};
            std::string_view previous;
            for (const auto &recipe : Recipes) {
                INFO("corpus v" << Version << " scene=" << recipe.id);
                CHECK_FALSE(recipe.id.empty());
                CHECK(previous < recipe.id);
                previous = recipe.id;
                CHECK(recipe.controllers > 0);
                CHECK(recipe.controllers <= MaximumControllers);
                CHECK(recipe.ticks > 0);
                CHECK(recipe.ticks <= MaximumTicks);
                const auto kind = static_cast<std::size_t>(recipe.scene);
                REQUIRE(kind < kinds.size());
                CHECK_FALSE(kinds[kind]);
                kinds[kind] = true;
                CHECK(Find(recipe.scene) == &recipe);
                CHECK(recipe.positionToleranceMeters > 0);
                CHECK(recipe.replayToleranceMeters > 0);
            }
            CHECK(std::ranges::all_of(kinds, [](bool present) {
                return present;
            }));
        }

#if HORO_TEST_PHYSICS_NATIVE
        using namespace TestDetail;
        using namespace ReferenceTest;

        /** @brief Normalized outcome excludes native/process identities while retaining tick and logical controller ordinal. */
        struct Observation final {
            std::uint64_t tick{};
            std::uint32_t controller{};
            Math::Vec3 position;
            Math::Vec3 velocity;
            bool grounded{};
            bool attachedToOwnedBody{};
            CharacterMovementTermination termination{};
        };

        struct Trace final {
            std::array<Observation, MaximumControllers * MaximumTicks> values{};
            std::size_t count{};
        };

        /** @brief Owns actual resident native geometry and a bounded multi-controller query world, with no renderer/assets. */
        struct NativeScene final {
            const Recipe &recipe;
            std::unique_ptr<Physics::PhysicsRuntime> runtime;
            std::unique_ptr<Physics::PhysicsWorld> physics;
            std::unique_ptr<CharacterWorld> character;
            std::unique_ptr<CharacterPhysicsQueryAdapter> adapter;
            std::array<CharacterControllerHandle, MaximumControllers> controllers{};
            std::optional<Physics::BodyHandle> body;
            Physics::CollisionLayerId layer;
            Physics::CollisionProfileId profile;
            Physics::PhysicsQueryChannelId channel;

            NativeScene(const Recipe &source, const bool reverse) : recipe(source) {
                auto settings = Physics::Test::SmallWorldSettings().Values();
                settings.world.gravity = {};
                const auto policy = ControllerDescriptor({61, WorldId(), PhysicsWorldId(), 1, 101, 1});
                profile = policy.collisionProfile;
                channel = policy.queryChannel;
                layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value();
                Physics::ProjectCollisionSchema
                    schema{.defaultProfile = profile,
                           .layers = {{layer}},
                           .pairs = {{layer, layer, Physics::SimulationPairResponse::Block}},
                           .queryChannels = {{channel}},
                           .profiles = {{profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}}}};
                auto normalized = Physics::NormalizedCollisionSchema::Create(schema);
                REQUIRE(normalized.HasValue());
                runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value();
                physics = runtime
                              ->PrepareWorld(Physics::PhysicsWorldSettings::Capture(settings).Value(),
                                             {std::make_shared<const Physics::NormalizedCollisionSchema>(std::move(normalized).Value()), 1})
                              .Value();
                REQUIRE(physics->Activate(PhysicsWorldId()).HasValue());
                if (reverse)
                    AddResidentBody();
                if (!Platform()) {
                    REQUIRE(physics
                                ->CreateQueryFixture({.shape = Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0},
                                                      .layer = layer,
                                                      .profile = profile,
                                                      .channel = channel})
                                .HasValue());
                }
                if (!reverse)
                    AddResidentBody();
                REQUIRE(physics
                            ->AdvanceFixedTick(
                                {.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
                auto owner = WorldDescriptor();
                owner.collisionFilterGeneration = 1;
                owner.physicsSnapshotRevision = physics->PublishedTick().publicationRevision;
                CharacterWorldSettingsDescriptor configuration;
                configuration.capacities.maximumControllers = recipe.controllers;
                configuration.work.maximumQueriesPerTick = 512;
                configuration.work.maximumCommandsPerTick = 32;
                character = CharacterWorld::Prepare(owner, CharacterWorldSettings::Capture(configuration).Value()).Value();
                for (std::uint32_t ordinal = 0; ordinal < recipe.controllers; ++ordinal) {
                    auto descriptor = ControllerDescriptor(character->Descriptor());
                    descriptor.capsule = {0.25F, 0.5F};
                    descriptor.gravity = {};
                    descriptor.collisionRootPosition = Start(ordinal);
                    descriptor.inheritPlatformHeading = Platform();
                    controllers[ordinal] = character->CreateController(descriptor).Value();
                }
                REQUIRE(character->Activate().HasValue());
                adapter = std::make_unique<CharacterPhysicsQueryAdapter>(*physics);
                for (std::uint32_t ordinal = 0; ordinal < recipe.controllers; ++ordinal)
                    REQUIRE(character->SpawnController(controllers[ordinal], Query(0)).HasValue());
            }

            bool Platform() const {
                return recipe.semantics == Semantics::PredictedKinematicSupport;
            }

            bool Dynamic() const {
                return recipe.semantics == Semantics::DynamicObstacleOnly;
            }

            Math::Vec3 Start(const std::uint32_t ordinal) const {
                if (Platform())
                    return {1, 1.02F, 0};
                if (recipe.scene == ReferenceScene::Crowd)
                    return {ordinal % 2 == 0 ? -0.25F : 0.25F, 0.77F, static_cast<float>(ordinal / 2) * 2};
                return {0, 0.77F, 0};
            }

            void AddResidentBody() {
                if (!Platform() && !Dynamic())
                    return;
                const auto shape =
                    physics->CreateSceneShape(Physics::PhysicsBoxShape{Platform() ? Math::Vec3{4, 0.25F, 4} : Math::Vec3{0.25F, 2, 2}});
                REQUIRE(shape.HasValue());
                Physics::PhysicsBodyDescriptor descriptor;
                descriptor.shape = shape.Value();
                descriptor.motion = Platform() ? Physics::PhysicsMotionType::Kinematic : Physics::PhysicsMotionType::Dynamic;
                descriptor.mass =
                    Platform() ? Physics::PhysicsMassPolicy{Physics::PhysicsNoMass{}} : Physics::PhysicsMassPolicy{Physics::PhysicsMass{1}};
                if (Dynamic())
                    descriptor.pose.translation = {1, 2, 0};
                Physics::PhysicsSceneCollisionBinding collision{.profile = profile,
                                                                .colliders = {
                                                                    {{}, Physics::PhysicsShapeSubresourceId::FromValue(11), {Material()}}}};
                const auto added = physics->CreateSceneBody({.body = descriptor,
                                                             .initialActivity = recipe.scene == ReferenceScene::DynamicSleeping
                                                                                    ? Physics::PhysicsInitialBodyActivity::Sleeping
                                                                                    : Physics::PhysicsInitialBodyActivity::Awake,
                                                             .collision = std::move(collision)});
                REQUIRE(added.HasValue());
                body = added.Value();
            }

            CharacterPhysicsQueryContext Query(const std::uint64_t tick) {
                return adapter->Context(QueryExpectations(character->Descriptor(), tick));
            }

            Physics::PhysicsKinematicTargetCommand Target(const std::uint64_t tick) const {
                Physics::PhysicsPose pose;
                if (recipe.scene == ReferenceScene::PlatformTranslation)
                    pose.translation.x = static_cast<float>(tick - 1) * 0.25F;
                else
                    pose.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, static_cast<float>(tick - 1) * Math::Pi * 0.5F);
                return {{Physics::PhysicsTransformAuthorityProtocolVersion, tick, 61, *body,
                         Physics::PhysicsCommandSourceId::Create(1).Value(), tick},
                        pose};
            }

            Math::Vec3 Expected(const std::uint64_t tick, const std::uint32_t ordinal) const {
                auto expected = Start(ordinal);
                if (recipe.scene == ReferenceScene::PlatformTranslation)
                    expected.x += static_cast<float>(tick - 1) * 0.25F;
                else if (recipe.scene == ReferenceScene::PlatformRotation) {
                    // Quarter turns have exact independently authored axis endpoints, not observed native golds.
                    constexpr std::array positions{Math::Vec3{1, 1.02F, 0}, Math::Vec3{0, 1.02F, -1}, Math::Vec3{-1, 1.02F, 0}};
                    expected = positions[tick - 1];
                } else if (Dynamic())
                    expected.x = 0.48F;
                else
                    expected.x +=
                        static_cast<float>(tick) * 0.1F * (recipe.scene == ReferenceScene::Crowd && ordinal % 2 != 0 ? -1.0F : 1.0F);
                return expected;
            }

            /** @brief Physics alone integrates the admitted platform motion and publishes the next query frame. */
            void CompletePlatformPhysics(const std::uint64_t tick) {
                constexpr auto quantum = 16'666'667;
                constexpr float seconds = static_cast<float>(quantum) / 1'000'000'000.0F;
                Physics::PhysicsBodyMutation mutation{.body = *body};
                mutation.linearVelocity =
                    recipe.scene == ReferenceScene::PlatformTranslation ? Math::Vec3{0.25F / seconds, 0, 0} : Math::Vec3{};
                mutation.angularVelocity =
                    recipe.scene == ReferenceScene::PlatformRotation ? Math::Vec3{0, Math::Pi * 0.5F / seconds, 0} : Math::Vec3{};
                // Physics command source sequences restart at one inside each canonical tick frame.
                const auto admitted =
                    physics->QueueStructuralCommand({.order = {.simulationTick = tick,
                                                               .worldGeneration = physics->Identity().Value(),
                                                               .sceneGeneration = 61,
                                                               .targetKind = Physics::PhysicsCommandTargetKind::Body,
                                                               .targetIdentity = static_cast<std::uint64_t>(body->slot.index) + 1,
                                                               .commandKind = Physics::PhysicsStructuralCommandKind::Change,
                                                               .source = Physics::PhysicsCommandSourceId::Create(1).Value(),
                                                               .sourceSequence = 1},
                                                     .bodyMutation = mutation});
                if (admitted.HasError())
                    UNSCOPED_INFO(admitted.ErrorValue().code.Value() << ": " << admitted.ErrorValue().message);
                REQUIRE(admitted.HasValue());
                const auto stepped = physics->AdvanceFixedTick(
                    {.simulationTick = tick, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(quantum)});
                if (stepped.HasError())
                    UNSCOPED_INFO(stepped.ErrorValue().code.Value() << ": " << stepped.ErrorValue().message);
                REQUIRE(stepped.HasValue());
                const auto actual = physics->ReadSceneBodyReconciliation(*body).Value().state.pose;
                const auto expected = Target(tick).targetPose;
                CHECK(actual.translation.x == Catch::Approx(expected.translation.x).margin(recipe.positionToleranceMeters));
                CHECK(actual.translation.y == Catch::Approx(expected.translation.y).margin(recipe.positionToleranceMeters));
                CHECK(actual.translation.z == Catch::Approx(expected.translation.z).margin(recipe.positionToleranceMeters));
                const auto heading = actual.rotation.Rotate({0, 0, 1});
                const auto expectedHeading = expected.rotation.Rotate({0, 0, 1});
                CHECK(heading.x == Catch::Approx(expectedHeading.x).margin(recipe.positionToleranceMeters));
                CHECK(heading.y == Catch::Approx(expectedHeading.y).margin(recipe.positionToleranceMeters));
                CHECK(heading.z == Catch::Approx(expectedHeading.z).margin(recipe.positionToleranceMeters));
                REQUIRE(character->RefreshPhysicsSnapshot(physics->Identity(), physics->PublishedTick().publicationRevision).HasValue());
            }

            Trace Run(const bool reverse) {
                Trace trace;
                const auto initial = body ? std::optional{physics->ReadSceneBodyReconciliation(*body).Value().state} : std::nullopt;
                if (Dynamic()) {
                    REQUIRE(initial->activity == (recipe.scene == ReferenceScene::DynamicSleeping ? Physics::PhysicsBodyActivity::Sleeping
                                                                                                  : Physics::PhysicsBodyActivity::Awake));
                    REQUIRE(physics->ReadSceneBodyPolicy(*body).Value().motion == Physics::PhysicsMotionType::Dynamic);
                }
                if (Platform())
                    REQUIRE(physics->ReadSceneBodyPolicy(*body).Value().motion == Physics::PhysicsMotionType::Kinematic);
                for (std::uint64_t tick = 1; tick <= recipe.ticks; ++tick) {
                    INFO("corpus v" << Version << " scene=" << recipe.id << " tick=" << tick);
                    const auto before = body ? std::optional{physics->ReadSceneBodyReconciliation(*body).Value().state} : std::nullopt;
                    const auto physicsPublication = physics->PublishedTick();
                    for (std::uint32_t index = 0; index < recipe.controllers; ++index) {
                        const auto ordinal = reverse ? recipe.controllers - index - 1 : index;
                        auto request = Movement(controllers[ordinal], tick, tick);
                        const float speed = Dynamic()                                                   ? 10.0F
                                            : Platform()                                                ? 0.0F
                                            : recipe.scene == ReferenceScene::Crowd && ordinal % 2 != 0 ? -1.0F
                                                                                                        : 1.0F;
                        request.desiredVelocityMetersPerSecond = Math::Vec3{speed, 0, 0};
                        REQUIRE(character->QueueMovementCommand(request).HasValue());
                    }
                    CharacterMetricCapture capture;
                    auto input = FixedTick(tick);
                    input.fixedDelta = Duration::FromNanoseconds(Platform() ? 16'666'667 : 100'000'000);
                    input.query = Query(tick);
                    input.metrics = &capture;
                    std::array<Physics::PhysicsKinematicTargetCommand, 1> targets{};
                    if (Platform() && tick > 1) {
                        targets[0] = Target(tick);
                        input.platformTargets = targets;
                    }
                    const auto advanced = character->AdvanceFixedTick(input);
                    if (advanced.HasError())
                        UNSCOPED_INFO(advanced.ErrorValue().message);
                    REQUIRE(advanced.HasValue());
                    RequireMovementBudget(*character, capture);
                    REQUIRE(capture.snapshot.queries > 0);
                    REQUIRE_FALSE(capture.snapshot.failed);
                    REQUIRE(character->PublishedTick().completedTick == tick);
                    for (std::uint32_t ordinal = 0; ordinal < recipe.controllers; ++ordinal) {
                        INFO("controller ordinal=" << ordinal);
                        const auto snapshot = character->ControllerLocomotionSnapshot(controllers[ordinal]).Value();
                        REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, character->ControllerDescriptor(controllers[ordinal]).Value())
                                    .HasValue());
                        const auto expected = Expected(tick, ordinal);
                        CHECK(snapshot.transform.position.x == Catch::Approx(expected.x).margin(recipe.positionToleranceMeters));
                        CHECK(snapshot.transform.position.y == Catch::Approx(expected.y).margin(recipe.positionToleranceMeters));
                        CHECK(snapshot.transform.position.z == Catch::Approx(expected.z).margin(recipe.positionToleranceMeters));
                        CHECK(snapshot.movement.grounded);
                        CHECK(snapshot.movement.termination == CharacterMovementTermination::Complete);
                        CHECK(snapshot.movement.sequence == tick);
                        if (Platform()) {
                            CHECK(snapshot.movement.groundBody == body);
                            CHECK(snapshot.movement.platformAttached);
                            CHECK_FALSE(snapshot.movement.platformCarryRejected);
                            if (recipe.scene == ReferenceScene::PlatformRotation) {
                                constexpr std::array headings{Math::Vec3{0, 0, 1}, Math::Vec3{1, 0, 0}, Math::Vec3{0, 0, -1}};
                                const auto heading = snapshot.transform.heading.Rotate({0, 0, 1});
                                CHECK(heading.x == Catch::Approx(headings[tick - 1].x).margin(recipe.positionToleranceMeters));
                                CHECK(heading.y == Catch::Approx(headings[tick - 1].y).margin(recipe.positionToleranceMeters));
                                CHECK(heading.z == Catch::Approx(headings[tick - 1].z).margin(recipe.positionToleranceMeters));
                            }
                        }
                        REQUIRE(trace.count < trace.values.size());
                        trace.values[trace.count++] = {tick,
                                                       ordinal,
                                                       snapshot.transform.position,
                                                       snapshot.movement.achievedVelocityMetersPerSecond,
                                                       snapshot.movement.grounded,
                                                       body && snapshot.movement.groundBody == body,
                                                       snapshot.movement.termination};
                    }
                    if (body) {
                        const auto after = physics->ReadSceneBodyReconciliation(*body).Value().state;
                        CHECK(after.body == before->body);
                        CHECK(after.pose == before->pose);
                        CHECK(after.linearVelocity == before->linearVelocity);
                        CHECK(after.angularVelocity == before->angularVelocity);
                        CHECK(after.activity == before->activity);
                    }
                    CHECK(physics->PublishedTick().publicationRevision == physicsPublication.publicationRevision);
                    CHECK(physics->PublishedTick().completedTick == physicsPublication.completedTick);
                    if (Platform() && tick > 1)
                        CompletePlatformPhysics(tick);
                }
                return trace;
            }
        };

        Trace Run(const Recipe &recipe, const bool reverse) {
            if (static_cast<unsigned>(recipe.scene) < static_cast<unsigned>(ReferenceScene::Flat)) {
                const auto outcome = RunReference(recipe.scene, reverse);
                Trace trace;
                trace.values[trace.count++] =
                    {1, 0, outcome.position, outcome.velocity, outcome.grounded, false, CharacterMovementTermination::Complete};
                return trace;
            }
            NativeScene scene{recipe, reverse};
            return scene.Run(reverse);
        }

        /** @brief Fails one actual query boundary without substituting geometry or mutating the native provider. */
        struct CancelledQuery final {
            CharacterPhysicsQueryContext original;
            unsigned calls{};

            CharacterPhysicsQueryContext Context() {
                auto query = original;
                query.context = this;
                query.overlap = [](void *context, const CharacterOverlapProbeRequest &request) noexcept {
                    auto &self = *static_cast<CancelledQuery *>(context);
                    return self.original.overlap(self.original.context, request);
                };
                query.sweep = [](void *context, const CharacterSweepProbeRequest &) noexcept {
                    ++static_cast<CancelledQuery *>(context)->calls;
                    return Result<CharacterSweepProbeResult>::Failure(MakeError(Physics::PhysicsErrors::QueryCancelled));
                };
                query.platformBody = [](void *context, const Physics::BodyHandle body, const Physics::ShapeHandle shape,
                                        const std::uint64_t revision) noexcept {
                    auto &self = *static_cast<CancelledQuery *>(context);
                    return self.original.platformBody(self.original.context, body, shape, revision);
                };
                return query;
            }
        };

        TEST_CASE("Character reference query cancellation retains the complete prior multi-tick publication",
                  "[physics][native][character-reference][cancellation]") {
            NativeScene scene{*Find(ReferenceScene::Flat), false};
            scene.Run(false);
            const auto before = scene.character->ControllerLocomotionSnapshot(scene.controllers[0]).Value();
            const auto publication = scene.character->PublishedTick();
            auto command = Movement(scene.controllers[0], 5, 5);
            command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            REQUIRE(scene.character->QueueMovementCommand(command).HasValue());
            CancelledQuery cancelled{scene.Query(5)};
            auto input = FixedTick(5);
            input.fixedDelta = Duration::FromNanoseconds(100'000'000);
            input.query = cancelled.Context();
            RequireError(scene.character->AdvanceFixedTick(input), Physics::PhysicsErrors::QueryCancelled);
            CHECK(cancelled.calls == 1);
            CHECK(scene.character->PublishedTick() == publication);
            const auto after = scene.character->ControllerLocomotionSnapshot(scene.controllers[0]).Value();
            CHECK(after.stateRevision == before.stateRevision);
            CHECK(after.transform.position == before.transform.position);
            CHECK(after.movement.achievedVelocityMetersPerSecond == before.movement.achievedVelocityMetersPerSecond);
            CHECK(after.movement.sequence == before.movement.sequence);
            const auto descriptor = scene.character->ControllerDescriptor(scene.controllers[0]).Value();
            scene.character->Shutdown();
            scene.physics->Shutdown();
            CHECK(ValidateCharacterLocomotionSnapshot(before, descriptor).HasValue());
        }

        TEST_CASE("Versioned Character reference scenes match authored outcomes and admission-order replay",
                  "[physics][native][character-reference]") {
            const auto index = GENERATE(Catch::Generators::range(std::size_t{0}, Recipes.size()));
            const auto &recipe = Recipes[index];
            INFO("corpus v" << Version << " scene=" << recipe.id);
            const auto first = Run(recipe, false);
            const auto replay = Run(recipe, true);
            REQUIRE(first.count == recipe.controllers * recipe.ticks);
            REQUIRE(replay.count == first.count);
            for (std::size_t index = 0; index < first.count; ++index) {
                const auto &a = first.values[index];
                const auto &b = replay.values[index];
                INFO("tick=" << a.tick << " controller=" << a.controller);
                CHECK(a.tick == b.tick);
                CHECK(a.controller == b.controller);
                CHECK(a.position.x == Catch::Approx(b.position.x).margin(recipe.replayToleranceMeters));
                CHECK(a.position.y == Catch::Approx(b.position.y).margin(recipe.replayToleranceMeters));
                CHECK(a.position.z == Catch::Approx(b.position.z).margin(recipe.replayToleranceMeters));
                CHECK(a.velocity.x == Catch::Approx(b.velocity.x).margin(recipe.replayToleranceMeters));
                CHECK(a.velocity.y == Catch::Approx(b.velocity.y).margin(recipe.replayToleranceMeters));
                CHECK(a.velocity.z == Catch::Approx(b.velocity.z).margin(recipe.replayToleranceMeters));
                CHECK(a.grounded == b.grounded);
                CHECK(a.attachedToOwnedBody == b.attachedToOwnedBody);
                CHECK(a.termination == b.termination);
            }
        }
#else
        TEST_CASE("Native Character reference execution reports the unavailable composition", "[physics][character-reference]") {
            SKIP("Canonical native Physics is not compiled; manifest and explicit Null rejection run, native movement is unqualified.");
        }
#endif
    }  // namespace
}  // namespace Horo::Character
