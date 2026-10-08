#include "CharacterWorldTestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        // A geometric plane fixture answers casts from actual positions and directions rather
        // than scripted call indices, so successive ticks exercise the committed movement path.
        struct RampProbe final {
            Math::Vec3 normal;
            bool malformed{};
            bool shutdownDuringSweep{};
            CharacterWorld *world{};
            std::uint32_t calls{};
            std::optional<Math::Vec3> coincidentNormal;
            bool reverseHits{};
            bool coincidentTrigger{};
            std::optional<float> downhillWallX;

            static Result<CharacterOverlapProbeResult> Overlap(void *, const CharacterOverlapProbeRequest &) noexcept {
                return Result<CharacterOverlapProbeResult>::Success({});
            }

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &probe = *static_cast<RampProbe *>(context);
                ++probe.calls;
                if (probe.shutdownDuringSweep)
                    probe.world->Shutdown();
                CharacterSweepProbeResult result;
                const float closing = -Math::Dot(request.direction, probe.normal);
                if (closing > 1.0e-5F) {
                    const float distance = std::max(0.0F, Math::Dot(request.position, probe.normal) / closing);
                    if (distance <= request.maximumDistanceMeters) {
                        result.hitCount = 1;
                        result.hits[0] = {.shape = {request.physicsWorld, {9, 3}},
                                          .point = request.position + request.direction * distance,
                                          .normal = probe.malformed ? Math::Vec3{} : probe.normal,
                                          .response = Physics::PhysicsQueryResponse::Block,
                                          .distanceMeters = distance};
                        if (probe.coincidentNormal.has_value() && (request.direction.x > 0.0F || probe.coincidentTrigger)) {
                            result.hits[1] = result.hits[0];
                            result.hits[1].shape.slot.index = 10;
                            result.hits[1].normal = *probe.coincidentNormal;
                            result.hits[1].trigger = probe.coincidentTrigger;
                            if (probe.coincidentTrigger)
                                result.hits[1].distanceMeters *= 0.5F;
                            result.hitCount = 2;
                            if (probe.reverseHits)
                                std::swap(result.hits[0], result.hits[1]);
                        }
                    }
                }
                if (probe.downhillWallX.has_value() && request.direction.x < -1.0e-5F) {
                    const float distance = std::max(0.0F, (*probe.downhillWallX - request.position.x) / request.direction.x);
                    if (distance <= request.maximumDistanceMeters) {
                        result.hits[result.hitCount++] = {.shape = {request.physicsWorld, {11, 3}},
                                                          .point = request.position + request.direction * distance,
                                                          .normal = {1, 0, 0},
                                                          .response = Physics::PhysicsQueryResponse::Block,
                                                          .distanceMeters = distance};
                    }
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &descriptor,
                                                               const std::uint64_t tick) noexcept {
                return {descriptor.sceneGeneration,
                        descriptor.identity,
                        descriptor.physicsWorld,
                        this,
                        Overlap,
                        descriptor.collisionFilterGeneration,
                        descriptor.originGeneration,
                        tick,
                        descriptor.physicsSnapshotRevision,
                        Sweep};
            }
        };

        [[nodiscard]] SpawnedActiveWorld RampWorld(const CharacterSteepSlopePolicy policy = CharacterSteepSlopePolicy::Stop,
                                                   const Math::Vec3 up = {0, 1, 0}, const bool preserveSpeed = true,
                                                   const std::uint32_t maximumMovementIterations = 8) {
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            settings.work.maximumMovementIterations = maximumMovementIterations;
            auto prepared = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value());
            REQUIRE(prepared.HasValue());
            SpawnedActiveWorld spawned{std::move(prepared).Value(), {}};
            auto descriptor = ControllerDescriptor(spawned.world->Descriptor());
            descriptor.collisionRootPosition = up * descriptor.skinWidthMeters;
            descriptor.up = up;
            descriptor.gravity = up * -9.81F;
            descriptor.steepSlopePolicy = policy;
            descriptor.preserveHorizontalSpeedOnSlopes = preserveSpeed;
            spawned.controller = spawned.world->CreateController(descriptor).Value();
            REQUIRE(spawned.world->Activate().HasValue());
            OverlapProbe probe;
            REQUIRE(spawned.world->SpawnController(spawned.controller, probe.Context(spawned.world->Descriptor())).HasValue());
            return spawned;
        }

        [[nodiscard]] CharacterLocomotionSnapshot MoveRamp(SpawnedActiveWorld &spawned, RampProbe &probe, const Math::Vec3 velocity,
                                                           const std::uint64_t tick = 1, const std::int64_t deltaNanoseconds = 16'666'667) {
            auto input = FixedTick(tick);
            input.fixedDelta = Duration::FromNanoseconds(deltaNanoseconds);
            input.query = probe.Context(spawned.world->Descriptor(), tick);
            auto command = Movement(spawned.controller, tick, tick);
            command.desiredVelocityMetersPerSecond = velocity;
            REQUIRE(spawned.world->QueueMovementCommand(command).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller);
            REQUIRE(snapshot.HasValue());
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot.Value(), spawned.world->ControllerDescriptor(spawned.controller).Value())
                        .HasValue());
            return snapshot.Value();
        }

        [[nodiscard]] Math::Vec3 RampNormal(const float degrees) {
            const float radians = degrees * Math::Pi / 180.0F;
            return {-std::sin(radians), std::cos(radians), 0};
        }

        TEST_CASE("Character preserves horizontal ramp speed through committed fixed ticks including the slope boundary",
                  "[physics][character][slope]") {
            const float degrees = GENERATE(0.0F, 15.0F, 30.0F, 45.0F);
            auto spawned = RampWorld();
            RampProbe probe{RampNormal(degrees)};
            for (std::uint64_t tick = 1; tick <= 20; ++tick) {
                const auto snapshot = MoveRamp(spawned, probe, {6, 0, 2}, tick);
                REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x == Catch::Approx(6).margin(1.0e-3F));
                REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.z == Catch::Approx(2).margin(1.0e-3F));
                REQUIRE(snapshot.movement.grounded);
                REQUIRE(snapshot.transform.position == snapshot.movement.finalPosition);
            }
            REQUIRE(probe.calls <= 20 * (Settings().Values().work.maximumMovementIterations + 1));
        }

        TEST_CASE("Character retains horizontal downhill speed and uses the descriptor up basis", "[physics][character][slope]") {
            SECTION("downhill") {
                auto spawned = RampWorld();
                RampProbe probe{RampNormal(30)};
                const auto snapshot = MoveRamp(spawned, probe, {-6, 0, 0});
                REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x == Catch::Approx(-6).margin(1.0e-3F));
                REQUIRE(snapshot.movement.finalPosition.y < 0.02F);
                REQUIRE(snapshot.movement.grounded);
            }
            SECTION("Z up") {
                auto spawned = RampWorld(CharacterSteepSlopePolicy::Stop, {0, 0, 1});
                const auto normal = RampNormal(30);
                RampProbe probe{{normal.x, 0, normal.y}};
                const auto snapshot = MoveRamp(spawned, probe, {6, 0, 0});
                REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x == Catch::Approx(6).margin(1.0e-3F));
                REQUIRE(snapshot.movement.finalPosition.z > 0.02F);
                REQUIRE(snapshot.movement.grounded);
            }
        }

        TEST_CASE("Character steep planes block uphill projection while permitting contour travel", "[physics][character][slope]") {
            const float degrees = GENERATE(45.01F, 60.0F, 89.99F);
            auto spawned = RampWorld();
            RampProbe probe{RampNormal(degrees)};
            const auto snapshot = MoveRamp(spawned, probe, {6, 0, 3});
            REQUIRE(snapshot.movement.finalPosition.y <= 0.02001F);
            REQUIRE(snapshot.movement.finalPosition.x < 0.021F);
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.z == Catch::Approx(3).margin(1.0e-3F));
            REQUIRE_FALSE(snapshot.movement.grounded);
        }

        TEST_CASE("Character mixed ramp and steep normals cannot retain ascent generated by ramp projection",
                  "[physics][character][slope][ordering]") {
            const bool reverse = GENERATE(false, true);
            auto spawned = RampWorld();
            RampProbe probe{RampNormal(30)};
            probe.coincidentNormal = RampNormal(60);
            probe.reverseHits = reverse;
            const auto snapshot = MoveRamp(spawned, probe, {6, 0, 0});
            REQUIRE(snapshot.movement.finalPosition.y <= 0.02001F);
            REQUIRE(snapshot.movement.finalPosition.x < 0.02F);
        }

        TEST_CASE("Character slope classification excludes nearer trigger normals before steep policy",
                  "[physics][character][slope][filter]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe probe{RampNormal(30)};
            probe.coincidentNormal = RampNormal(60);
            probe.coincidentTrigger = true;
            const auto snapshot = MoveRamp(spawned, probe, {6, 0, 0});
            REQUIRE(snapshot.movement.grounded);
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x == Catch::Approx(6).margin(1.0e-3F));
            REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            for (std::uint32_t index{}; index < snapshot.movement.contactCount; ++index)
                REQUIRE(snapshot.movement.contacts[index].shape.slot.index != 10);
        }

        TEST_CASE("Character stop and gravity slide are explicit deterministic fixed tick policies", "[physics][character][slope]") {
            auto stopped = RampWorld();
            RampProbe stopProbe{RampNormal(60)};
            const auto stop = MoveRamp(stopped, stopProbe, {});
            REQUIRE(stop.movement.finalPosition == Math::Vec3{0, 0.02F, 0});
            auto sliding = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe slideProbe{RampNormal(60)};
            const auto slide = MoveRamp(sliding, slideProbe, {});
            const float seconds = static_cast<float>(FixedTick(1).fixedDelta.ToNanoseconds()) / 1'000'000'000.0F;
            const Math::Vec3 gravity{0, -9.81F, 0};
            const Math::Vec3 expected = Math::Vec3{0, 0.02F, 0} +
                                        (gravity - slideProbe.normal * Math::Dot(gravity, slideProbe.normal)) * (0.5F * seconds * seconds);
            REQUIRE(slide.movement.finalPosition.x == Catch::Approx(expected.x).margin(1.0e-6F));
            REQUIRE(slide.movement.finalPosition.y == Catch::Approx(expected.y).margin(1.0e-6F));
            REQUIRE_FALSE(slide.movement.grounded);
            auto replay = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe replayProbe{RampNormal(60)};
            REQUIRE(MoveRamp(replay, replayProbe, {}).movement.finalPosition == slide.movement.finalPosition);
            REQUIRE(slideProbe.calls <= Settings().Values().work.maximumMovementIterations + 2);
        }

        TEST_CASE("Character steep acceleration retains committed velocity and agrees across fixed tick partitions",
                  "[physics][character][slope][continuation]") {
            auto partitioned = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe probe{RampNormal(60)};
            const Math::Vec3 gravity{0, -9.81F, 0};
            const auto acceleration = gravity - probe.normal * Math::Dot(gravity, probe.normal);
            for (std::uint64_t tick = 1; tick <= 4; ++tick) {
                const auto snapshot = MoveRamp(partitioned, probe, {}, tick, 25'000'000);
                const float time = static_cast<float>(tick) * 0.025F;
                const auto expected = Math::Vec3{0, 0.02F, 0} + acceleration * (time * time * 0.5F);
                REQUIRE(snapshot.movement.finalPosition.x == Catch::Approx(expected.x).margin(1.0e-5F));
                REQUIRE(snapshot.movement.finalPosition.y == Catch::Approx(expected.y).margin(1.0e-5F));
                REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond.x == Catch::Approx(acceleration.x * time).margin(1.0e-5F));
                REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond.y == Catch::Approx(acceleration.y * time).margin(1.0e-5F));
            }
            auto single = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe singleProbe{RampNormal(60)};
            const auto reference = MoveRamp(single, singleProbe, {}, 1, 100'000'000);
            const auto actual = partitioned.world->ControllerLocomotionSnapshot(partitioned.controller).Value();
            REQUIRE(actual.movement.finalPosition.x == Catch::Approx(reference.movement.finalPosition.x).margin(1.0e-5F));
            REQUIRE(actual.movement.finalPosition.y == Catch::Approx(reference.movement.finalPosition.y).margin(1.0e-5F));
            REQUIRE(actual.movement.gravityVelocityMetersPerSecond.y ==
                    Catch::Approx(reference.movement.gravityVelocityMetersPerSecond.y).margin(1.0e-5F));
        }

        TEST_CASE("Character blocking geometry stops accumulated slide pressure", "[physics][character][slope][continuation][collision]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe probe{RampNormal(60)};
            probe.downhillWallX = -0.0001F;
            for (std::uint64_t tick = 1; tick <= 3; ++tick) {
                const auto snapshot = MoveRamp(spawned, probe, {}, tick);
                REQUIRE(snapshot.movement.finalPosition.x >= -0.0001F);
                REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            }
        }

        TEST_CASE("Character slide charges actual movement work and rejects exhausted iteration budgets transactionally",
                  "[physics][character][slope][capacity]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Slide, {0, 1, 0}, true, 1);
            RampProbe probe{RampNormal(60)};
            SECTION("stationary slide needs only one movement cast") {
                const auto snapshot = MoveRamp(spawned, probe, {});
                REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond.y < 0);
                REQUIRE(probe.calls == 3);
            }
            SECTION("ordinary contour travel uses the only available movement cast") {
                auto request = Movement(spawned.controller, 1, 1);
                request.desiredVelocityMetersPerSecond = Math::Vec3{0, 0, 3};
                auto input = FixedTick(1);
                input.query = probe.Context(spawned.world->Descriptor(), 1);
                REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
                RequireError(spawned.world->AdvanceFixedTick(input), CharacterErrors::CapacityExceeded);
                REQUIRE(spawned.world->PublishedTick().completedTick == 0);
                REQUIRE(spawned.world->ControllerLocomotionSnapshot(spawned.controller).HasError());
            }
        }

        TEST_CASE("Character failed slope tick preserves committed gravity continuation",
                  "[physics][character][slope][continuation][validation]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe probe{RampNormal(60)};
            const auto first = MoveRamp(spawned, probe, {});
            probe.malformed = true;
            auto input = FixedTick(2);
            input.query = probe.Context(spawned.world->Descriptor(), 2);
            REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 2, 2)).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasError());
            const auto retained = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
            REQUIRE(retained.tick == first.tick);
            REQUIRE(retained.movement.gravityVelocityMetersPerSecond == first.movement.gravityVelocityMetersPerSecond);
            probe.malformed = false;
            const auto resumed = MoveRamp(spawned, probe, {}, 3);
            REQUIRE(resumed.movement.gravityVelocityMetersPerSecond.y ==
                    Catch::Approx(first.movement.gravityVelocityMetersPerSecond.y * 2.0F).margin(1.0e-5F));
            auto malformed = resumed;
            malformed.movement.gravityVelocityMetersPerSecond.x = std::numeric_limits<float>::infinity();
            REQUIRE(
                ValidateCharacterLocomotionSnapshot(malformed, spawned.world->ControllerDescriptor(spawned.controller).Value()).HasError());
        }

        TEST_CASE("Character teleport and walkable transition reset steep gravity continuation",
                  "[physics][character][slope][continuation][lifecycle]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe probe{RampNormal(60)};
            const auto first = MoveRamp(spawned, probe, {});
            REQUIRE(first.movement.gravityVelocityMetersPerSecond.y < 0.0F);
            SECTION("teleport") {
                OverlapProbe placement;
                const CharacterTeleportRequest teleport{spawned.controller, 2, {0, 0.02F, 0}, Math::Quaternion::Identity()};
                REQUIRE(spawned.world->TeleportController(teleport, placement.Context(spawned.world->Descriptor(), 2)).HasValue());
                RequireError(spawned.world->QueueMovementCommand(Movement(spawned.controller, 2, 2)), CharacterErrors::CommandOrderInvalid);
                REQUIRE(spawned.world->AdvanceFixedTick(FixedTick(2)).HasValue());
                const auto restarted = MoveRamp(spawned, probe, {}, 3);
                REQUIRE(restarted.movement.gravityVelocityMetersPerSecond == first.movement.gravityVelocityMetersPerSecond);
                REQUIRE(restarted.movement.finalPosition == first.movement.finalPosition);
            }
            SECTION("walkable support") {
                probe.normal = {0, 1, 0};
                const auto grounded = MoveRamp(spawned, probe, {}, 2);
                REQUIRE(grounded.movement.grounded);
                REQUIRE(grounded.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            }
        }

        TEST_CASE("Character can opt out of ramp speed preservation without changing steep safety", "[physics][character][slope]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Stop, {0, 1, 0}, false);
            RampProbe probe{RampNormal(30)};
            const auto snapshot = MoveRamp(spawned, probe, {6, 0, 0});
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x < 6);
            REQUIRE(snapshot.movement.grounded);
        }

        TEST_CASE("Character malformed slope policy and evidence fail without publishing candidate movement",
                  "[physics][character][slope][validation]") {
            SECTION("unknown discriminator") {
                auto world = PreparedWorld();
                auto descriptor = ControllerDescriptor(world->Descriptor());
                descriptor.steepSlopePolicy = static_cast<CharacterSteepSlopePolicy>(255);
                RequireError(world->CreateController(descriptor), CharacterErrors::DescriptorInvalid);
            }
            SECTION("invalid normal") {
                auto spawned = RampWorld();
                RampProbe probe{RampNormal(30), true};
                auto input = FixedTick(1);
                input.query = probe.Context(spawned.world->Descriptor(), 1);
                REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 1, 1)).HasValue());
                RequireError(spawned.world->AdvanceFixedTick(input), CharacterErrors::DescriptorInvalid);
                REQUIRE(spawned.world->PublishedTick().completedTick == 0);
            }
        }

        TEST_CASE("Character slope shutdown cancels candidate work and detached snapshots survive teardown",
                  "[physics][character][slope][lifecycle]") {
            auto spawned = RampWorld(CharacterSteepSlopePolicy::Slide);
            RampProbe probe{RampNormal(60)};
            const auto snapshot = MoveRamp(spawned, probe, {});
            const auto descriptor = spawned.world->ControllerDescriptor(spawned.controller).Value();
            probe.shutdownDuringSweep = true;
            probe.world = spawned.world.get();
            auto input = FixedTick(2);
            input.query = probe.Context(spawned.world->Descriptor(), 2);
            REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 2, 2)).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasError());
            REQUIRE(spawned.world->State() == CharacterWorldState::Destroyed);
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
        }
    }  // namespace
}  // namespace Horo::Character
