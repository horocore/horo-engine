#include "AllocationProbe.h"
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
            std::optional<CharacterSweepHit> secondGround;
            bool reverseHits{};
            std::optional<CharacterSweepHit> wall;
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
                    if (probe.secondGround.has_value()) {
                        result.hits[1] = *probe.secondGround;
                        result.hitCount = 2;
                        if (probe.reverseHits)
                            std::swap(result.hits[0], result.hits[1]);
                    }
                }
                if (probe.wall.has_value() && request.direction.x > 0.5F) {
                    result.hits[0] = *probe.wall;
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

        TEST_CASE("Character discards trigger evidence even when an adapter labels it Block", "[physics][character][filter][grounding]") {
            auto spawned = SpawnedActiveWorldWithController();
            GroundProbe probe;
            probe.provideGround = true;
            probe.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.02F);
            probe.ground.trigger = true;
            probe.wall = SweepHit(spawned.world->Descriptor(), 10, {-1, 0, 0}, 0.005F);
            probe.wall->trigger = true;
            auto command = Movement(spawned.controller, 1, 1);
            command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            REQUIRE(spawned.world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
            const auto result = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value().movement;
            REQUIRE_FALSE(result.grounded);
            REQUIRE(result.collisions == CharacterCollisionFlags::None);
            REQUIRE(result.contactCount == 0);
            REQUIRE(result.finalPosition.x > 0.016F);
        }

        TEST_CASE("Character excludes copied body layer and profile mismatches from walls and ground",
                  "[physics][character][filter][grounding]") {
            for (const int selector : {0, 1, 2}) {
                auto spawned = SpawnedActiveWorldWithController();
                GroundProbe probe;
                probe.provideGround = true;
                probe.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.02F);
                probe.wall = SweepHit(spawned.world->Descriptor(), 10, {-1, 0, 0}, 0.005F);
                probe.wall->body = probe.ground.body;
                CharacterCollisionSelectors filter;
                if (selector == 0)
                    filter.excludedBody = probe.ground.body;
                else if (selector == 1) {
                    filter.requiredLayer = Physics::CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value();
                    probe.ground.layer = Physics::CollisionLayerId::Parse("22345678-1234-4234-8234-123456789abc").Value();
                    probe.wall->layer = probe.ground.layer;
                } else {
                    filter.requiredProfile = Physics::CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abc").Value();
                    probe.ground.profile = Physics::CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
                    probe.wall->profile = probe.ground.profile;
                }
                auto command = Movement(spawned.controller, 1, 1);
                command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
                command.filterChange = filter;
                REQUIRE(spawned.world->QueueMovementCommand(command).HasValue());
                auto input = FixedTick(1);
                input.query = probe.Context(spawned.world->Descriptor(), 1);
                REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
                const auto result = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value().movement;
                REQUIRE_FALSE(result.grounded);
                REQUIRE(result.collisions == CharacterCollisionFlags::None);
                REQUIRE(result.contactCount == 0);
                REQUIRE(result.finalPosition.x > 0.016F);
            }
        }

        TEST_CASE("Character rejects missing selected layer evidence before publishing movement",
                  "[physics][character][filter][grounding]") {
            auto spawned = SpawnedActiveWorldWithController();
            GroundProbe probe;
            probe.provideGround = true;
            probe.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.02F);
            auto command = Movement(spawned.controller, 1, 1);
            command.filterChange = CharacterCollisionSelectors{
                .requiredLayer = Physics::CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value()};
            REQUIRE(spawned.world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            RequireError(spawned.world->AdvanceFixedTick(input), CharacterErrors::DescriptorInvalid);
            REQUIRE(spawned.world->PublishedTick().completedTick == 0);
            REQUIRE_FALSE(spawned.world->ControllerDescriptor(spawned.controller).Value().selectors.requiredLayer);
        }

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

        [[nodiscard]] GroundProbe FlatSupport(const SpawnedActiveWorld &spawned) {
            return {.ground = SweepHit(spawned.world->Descriptor(), 9, {0, 1, 0}, 0.02F), .provideGround = true};
        }

        [[nodiscard]] CharacterLocomotionSnapshot ResolveSurfaceTick(SpawnedActiveWorld &spawned, GroundProbe &probe,
                                                                     const std::uint64_t tick, const std::uint64_t sequence) {
            auto input = FixedTick(tick);
            input.query = probe.Context(spawned.world->Descriptor(), tick);
            REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, tick, sequence)).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
            return spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
        }

        TEST_CASE("Character surface facts copy exact Physics child material and committed correlation",
                  "[physics][character][grounding][surface]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto probe = FlatSupport(spawned);
            probe.ground.point = {2, 0, 3};
            probe.ground.material = Material();
            probe.ground.material->assetGeneration = 27;
            probe.ground.subshape = Physics::PhysicsShapeSubresourceId::FromValue(8);
            const auto descriptor = spawned.world->ControllerDescriptor(spawned.controller).Value();
            const auto snapshot = ResolveSurfaceTick(spawned, probe, 1, 42);
            const auto allocationsBefore = Tests::AllocationProbe::Count();
            const auto result = BuildCharacterGroundSurfaceFact(snapshot, descriptor);
            const auto allocationsAfter = Tests::AllocationProbe::Count();
            REQUIRE(allocationsAfter == allocationsBefore);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            const auto &fact = *result.Value();
            REQUIRE(fact.controller == snapshot.controller);
            REQUIRE(fact.tick == 1);
            REQUIRE(fact.sequence == 42);
            REQUIRE(fact.stateRevision == snapshot.stateRevision);
            REQUIRE(fact.publicationRevision == snapshot.transform.publicationRevision);
            REQUIRE(fact.support.shape == probe.ground.shape);
            REQUIRE(fact.support.body == probe.ground.body);
            REQUIRE(fact.support.subshape == probe.ground.subshape);
            REQUIRE(fact.support.material.asset == probe.ground.material->asset);
            REQUIRE(fact.support.material.assetGeneration == 27);
            REQUIRE(fact.support.material.slot == probe.ground.material->slot);
            REQUIRE(fact.support.materialSource == CharacterMaterialSource::Query);
            REQUIRE(fact.support.point == probe.ground.point);
            REQUIRE(snapshot.movement.contacts[0].subshape == probe.ground.subshape);
            REQUIRE(snapshot.movement.contacts[0].materialSource == CharacterMaterialSource::Query);
        }

        TEST_CASE("Character missing physical evidence is an explicit fallback and never invents airborne support",
                  "[physics][character][grounding][surface]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto probe = FlatSupport(spawned);
            const auto descriptor = spawned.world->ControllerDescriptor(spawned.controller).Value();
            const auto grounded = ResolveSurfaceTick(spawned, probe, 1, 3);
            const auto fact = BuildCharacterGroundSurfaceFact(grounded, descriptor);
            REQUIRE(fact.HasValue());
            REQUIRE(fact.Value().has_value());
            REQUIRE(fact.Value()->support.materialSource == CharacterMaterialSource::DescriptorFallback);
            REQUIRE(fact.Value()->support.material.asset == descriptor.defaultMaterial.asset);
            REQUIRE(fact.Value()->support.material.assetGeneration == descriptor.defaultMaterial.assetGeneration);
            REQUIRE(grounded.movement.contacts[0].materialSource == CharacterMaterialSource::DescriptorFallback);
            probe.provideGround = false;
            const auto airborne = ResolveSurfaceTick(spawned, probe, 2, 4);
            const auto absent = BuildCharacterGroundSurfaceFact(airborne, descriptor);
            REQUIRE(absent.HasValue());
            REQUIRE_FALSE(absent.Value().has_value());
            REQUIRE_FALSE(airborne.movement.groundSubshape.has_value());
            REQUIRE(airborne.movement.groundMaterialSource == CharacterMaterialSource::Query);
            REQUIRE(airborne.movement.groundPoint == Math::Vec3{});
        }

        TEST_CASE("Character copied surface facts survive material replacement removal and world shutdown",
                  "[physics][character][grounding][surface][lifecycle]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto probe = FlatSupport(spawned);
            probe.ground.material = Material();
            const auto descriptor = spawned.world->ControllerDescriptor(spawned.controller).Value();
            const auto first = ResolveSurfaceTick(spawned, probe, 1, 1);
            const auto original = BuildCharacterGroundSurfaceFact(first, descriptor).Value();
            ++probe.ground.material->assetGeneration;
            const auto replaced = ResolveSurfaceTick(spawned, probe, 2, 2);
            REQUIRE(replaced.movement.groundMaterial->assetGeneration == original->support.material.assetGeneration + 1);
            REQUIRE(replaced.movement.finalPosition == first.movement.finalPosition);
            probe.ground.material.reset();
            const auto removed = ResolveSurfaceTick(spawned, probe, 3, 3);
            REQUIRE(removed.movement.groundMaterialSource == CharacterMaterialSource::DescriptorFallback);
            REQUIRE(removed.movement.finalPosition == first.movement.finalPosition);
            spawned.world->Shutdown();
            RequireError(spawned.world->ControllerLocomotionSnapshot(spawned.controller), CharacterErrors::InvalidState);
            const auto retained = BuildCharacterGroundSurfaceFact(first, descriptor);
            REQUIRE(retained.HasValue());
            REQUIRE(retained.Value()->tick == 1);
            REQUIRE(retained.Value()->support.material.assetGeneration == original->support.material.assetGeneration);
            REQUIRE(retained.Value()->support.shape == original->support.shape);
        }

        TEST_CASE("Character selects stable authored children independently of Physics traversal order",
                  "[physics][character][grounding][surface][ordering]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto probe = FlatSupport(spawned);
            probe.ground.subshape = Physics::PhysicsShapeSubresourceId::FromValue(8);
            probe.secondGround = probe.ground;
            probe.secondGround->subshape = Physics::PhysicsShapeSubresourceId::FromValue(3);
            const auto forward = ResolveSurfaceTick(spawned, probe, 1, 1);
            probe.reverseHits = true;
            const auto reverse = ResolveSurfaceTick(spawned, probe, 2, 2);
            REQUIRE(forward.movement.groundSubshape->Value() == 3);
            REQUIRE(reverse.movement.groundSubshape == forward.movement.groundSubshape);
            REQUIRE(reverse.movement.finalPosition == forward.movement.finalPosition);
        }

        TEST_CASE("Character ground facts retain selected support when the admitted contact prefix is full",
                  "[physics][character][grounding][surface][capacity]") {
            SpawnedActiveWorld spawned{PreparedWorld(1), {}};
            auto descriptor = ControllerDescriptor(spawned.world->Descriptor());
            descriptor.maximumContacts = 1;
            spawned.controller = spawned.world->CreateController(descriptor).Value();
            REQUIRE(spawned.world->Activate().HasValue());
            OverlapProbe placement;
            REQUIRE(spawned.world->SpawnController(spawned.controller, placement.Context(spawned.world->Descriptor())).HasValue());
            auto probe = FlatSupport(spawned);
            probe.ground.point = {2, 0, 3};
            probe.ground.subshape = Physics::PhysicsShapeSubresourceId::FromValue(8);
            probe.wall = SweepHit(spawned.world->Descriptor(), 10, {-1, 0, 0}, 0.02F);
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            auto request = Movement(spawned.controller, 1, 1);
            request.desiredVelocityMetersPerSecond = Math::Vec3{6, 0, 0};
            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
            REQUIRE(snapshot.movement.contactCount == 1);
            REQUIRE(snapshot.movement.truncated);
            REQUIRE(snapshot.movement.contacts[0].shape == probe.wall->shape);
            const auto fact = BuildCharacterGroundSurfaceFact(snapshot, descriptor);
            REQUIRE(fact.HasValue());
            REQUIRE(fact.Value()->support.shape == probe.ground.shape);
            REQUIRE(fact.Value()->support.subshape == probe.ground.subshape);
            REQUIRE(fact.Value()->support.point == probe.ground.point);
        }

        TEST_CASE("Character surface projection rejects mixed correlation malformed children and false fallback",
                  "[physics][character][grounding][surface][validation]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto probe = FlatSupport(spawned);
            const auto descriptor = spawned.world->ControllerDescriptor(spawned.controller).Value();
            const auto valid = ResolveSurfaceTick(spawned, probe, 1, 1);
            SECTION("mixed tick") {
                auto invalid = valid;
                ++invalid.tick;
                REQUIRE(BuildCharacterGroundSurfaceFact(invalid, descriptor).HasError());
            }
            SECTION("foreign world") {
                auto invalid = valid;
                invalid.movement.groundShape.world = PhysicsWorldId(999);
                REQUIRE(BuildCharacterGroundSurfaceFact(invalid, descriptor).HasError());
            }
            SECTION("malformed child") {
                auto invalid = valid;
                invalid.movement.groundSubshape = Physics::PhysicsShapeSubresourceId{};
                REQUIRE(BuildCharacterGroundSurfaceFact(invalid, descriptor).HasError());
            }
            SECTION("unknown source") {
                auto invalid = valid;
                invalid.movement.groundMaterialSource = static_cast<CharacterMaterialSource>(255);
                REQUIRE(BuildCharacterGroundSurfaceFact(invalid, descriptor).HasError());
            }
            SECTION("false fallback generation") {
                auto invalid = valid;
                ++invalid.movement.groundMaterial->assetGeneration;
                REQUIRE(BuildCharacterGroundSurfaceFact(invalid, descriptor).HasError());
            }
            SECTION("false contact fallback") {
                auto invalid = valid;
                ++invalid.movement.contacts[0].material.assetGeneration;
                REQUIRE(BuildCharacterGroundSurfaceFact(invalid, descriptor).HasError());
            }
            SECTION("malformed producer child") {
                probe.ground.subshape = Physics::PhysicsShapeSubresourceId{};
                auto input = FixedTick(2);
                input.query = probe.Context(spawned.world->Descriptor(), 2);
                REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 2, 2)).HasValue());
                REQUIRE(spawned.world->AdvanceFixedTick(input).HasError());
                REQUIRE(spawned.world->PublishedTick().completedTick == 1);
            }
        }
    }  // namespace
}  // namespace Horo::Character
