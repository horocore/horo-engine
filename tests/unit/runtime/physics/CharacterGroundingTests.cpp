#include "CharacterWorldTestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        [[nodiscard]] CharacterSweepHit SweepHit(const CharacterWorldDescriptor &world, const std::uint32_t shapeIndex,
                                                 const Math::Vec3 normal, const float distance) {
            return {.body = Physics::BodyHandle{world.physicsWorld, {shapeIndex, 2}},
                    .shape = Physics::ShapeHandle{world.physicsWorld, {shapeIndex, 3}},
                    .point = {},
                    .normal = normal,
                    .material = std::nullopt,
                    .response = Physics::PhysicsQueryResponse::Block,
                    .distanceMeters = distance};
        }

        struct GroundProbe final {
            CharacterSweepHit ground;
            bool provideGround{};
            std::uint32_t calls{};

            static Result<CharacterOverlapProbeResult> NoopOverlap(void *, const CharacterOverlapProbeRequest &) noexcept {
                return Result<CharacterOverlapProbeResult>::Success({});
            }

            static Result<CharacterSweepProbeResult> Run(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &probe = *static_cast<GroundProbe *>(context);
                ++probe.calls;
                CharacterSweepProbeResult result;
                if (probe.provideGround && request.direction.y < -0.5F) {
                    result.hits[0] = probe.ground;
                    result.hitCount = 1;
                }
                return Result<CharacterSweepProbeResult>::Success(std::move(result));
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        NoopOverlap,
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Run};
            }
        };

        TEST_CASE("Character classifies and stably snaps a stationary flat support with coherent evidence",
                  "[physics][character][world][grounding]") {
            auto spawned = SpawnedActiveWorldWithController();
            GroundProbe probe;
            probe.provideGround = true;
            probe.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.05F);
            probe.ground.relativeVelocityMetersPerSecond = {0.5F, 0.0F, -0.25F};

            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 1, 1)).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());

            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller);
            REQUIRE(snapshot.HasValue());
            const auto &movement = snapshot.Value().movement;
            REQUIRE(movement.grounded);
            REQUIRE(movement.collisions == CharacterCollisionFlags::Ground);
            REQUIRE(movement.groundBody == probe.ground.body);
            REQUIRE(movement.groundShape == probe.ground.shape);
            REQUIRE(movement.groundNormal == Math::Vec3{0, 1, 0});
            REQUIRE(movement.groundSlopeDegrees == Catch::Approx(0.0F));
            REQUIRE(movement.groundDistanceMeters == Catch::Approx(0.02F).margin(1.0e-5F));
            REQUIRE(movement.groundRelativeVelocityMetersPerSecond == probe.ground.relativeVelocityMetersPerSecond);
            REQUIRE(movement.finalPosition.y == Catch::Approx(-0.03F).margin(1.0e-5F));
            REQUIRE(movement.groundMaterial.has_value());
            REQUIRE(probe.calls == 1);
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot.Value(), spawned.world->ControllerDescriptor(spawned.controller).Value())
                        .HasValue());
        }

        TEST_CASE("Character ground classification is stable at the configured slope boundary",
                  "[physics][character][world][grounding][slope]") {
            const auto resolve = [](const Math::Vec3 normal) {
                auto spawned = SpawnedActiveWorldWithController();
                GroundProbe probe;
                probe.provideGround = true;
                probe.ground = SweepHit(spawned.world->Descriptor(), 9, normal, 0.02F);
                auto input = FixedTick(1);
                input.query = probe.Context(spawned.world->Descriptor(), 1);
                REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 1, 1)).HasValue());
                REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
                return spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value().movement;
            };

            const float diagonal = std::sqrt(0.5F);
            const auto boundary = resolve({diagonal, diagonal, 0.0F});
            REQUIRE(boundary.grounded);
            REQUIRE(boundary.groundSlopeDegrees == Catch::Approx(45.0F).margin(1.0e-3F));

            const float aboveBoundary = 45.01F * Math::Pi / 180.0F;
            const auto steep = resolve({std::sin(aboveBoundary), std::cos(aboveBoundary), 0.0F});
            REQUIRE_FALSE(steep.grounded);
            REQUIRE_FALSE(steep.groundShape.IsValid());
        }

        TEST_CASE("Character does not snap an upward-moving controller back to the floor",
                  "[physics][character][world][grounding][airborne]") {
            auto spawned = SpawnedActiveWorldWithController();
            GroundProbe probe;
            probe.provideGround = true;
            probe.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.05F);
            auto request = Movement(spawned.controller, 1, 1);
            request.desiredVelocityMetersPerSecond = Math::Vec3{0, 1, 0};
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());

            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller);
            REQUIRE(snapshot.HasValue());
            REQUIRE_FALSE(snapshot.Value().movement.grounded);
            REQUIRE_FALSE(snapshot.Value().movement.groundShape.IsValid());
            REQUIRE(snapshot.Value().movement.finalPosition.y > 0.0F);
            REQUIRE(probe.calls == 1);
        }

        TEST_CASE("Character rejects malformed ground identity and relative velocity before publication",
                  "[physics][character][world][grounding][validation]") {
            auto spawned = SpawnedActiveWorldWithController();
            GroundProbe probe;
            probe.provideGround = true;
            probe.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.02F);
            probe.ground.shape.world = PhysicsWorldId(999);
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 1, 1)).HasValue());
            RequireError(spawned.world->AdvanceFixedTick(input), CharacterErrors::DescriptorInvalid);
            REQUIRE((spawned.world->PublishedTick() == CharacterPublishedTick{}));
            RequireError(spawned.world->ControllerLocomotionSnapshot(spawned.controller), CharacterErrors::InvalidState);

            spawned.world->Shutdown();
            REQUIRE(spawned.world->State() == CharacterWorldState::Destroyed);
        }
    }  // namespace
}  // namespace Horo::Character
