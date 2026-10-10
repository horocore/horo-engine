#include "CharacterWorldTestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        struct AirProbe final {
            Math::Vec3 up{0, 1, 0};
            bool floor{true};
            std::optional<float> ceiling;
            bool malformed{};
            CharacterWorld *shutdownWorld{};
            std::uint32_t calls{};

            static Result<CharacterOverlapProbeResult> Overlap(void *, const CharacterOverlapProbeRequest &) noexcept {
                return Result<CharacterOverlapProbeResult>::Success({});
            }

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &probe = *static_cast<AirProbe *>(context);
                ++probe.calls;
                if (probe.shutdownWorld)
                    probe.shutdownWorld->Shutdown();
                CharacterSweepProbeResult result;
                const float direction = Math::Dot(request.direction, probe.up);
                const float height = Math::Dot(request.position, probe.up);
                const bool falling = direction < -1.0e-5F && probe.floor;
                const bool rising = direction > 1.0e-5F && probe.ceiling.has_value();
                if (!falling && !rising)
                    return Result<CharacterSweepProbeResult>::Success(result);
                const float distance = std::max(0.0F, ((falling ? 0.0F : *probe.ceiling) - height) / direction);
                if (distance <= request.maximumDistanceMeters) {
                    result.hitCount = 1;
                    result.hits[0] = {.shape = {request.physicsWorld, {9, 3}},
                                      .point = request.position + request.direction * distance,
                                      .normal = probe.malformed ? Math::Vec3{} : probe.up * (falling ? 1.0F : -1.0F),
                                      .response = Physics::PhysicsQueryResponse::Block,
                                      .distanceMeters = distance};
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        Overlap,
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Sweep};
            }
        };

        [[nodiscard]] SpawnedActiveWorld AirWorld(const Math::Vec3 up = {0, 1, 0}, const Math::Vec3 gravity = {0, -9.81F, 0}) {
            auto world = PreparedWorld(1);
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.up = up;
            descriptor.gravity = gravity;
            descriptor.collisionRootPosition = up * descriptor.skinWidthMeters;
            const auto controller = world->CreateController(descriptor).Value();
            REQUIRE(world->Activate().HasValue());
            OverlapProbe overlap;
            REQUIRE(world->SpawnController(controller, overlap.Context(world->Descriptor())).HasValue());
            return {std::move(world), controller};
        }

        [[nodiscard]] CharacterLocomotionSnapshot MoveAir(SpawnedActiveWorld &host, AirProbe &probe, const std::uint64_t tick,
                                                          const bool jump = false, const std::int64_t nanos = 16'666'667) {
            auto command = Movement(host.controller, tick, tick);
            command.jumpRequested = jump;
            auto input = FixedTick(tick);
            input.fixedDelta = Duration::FromNanoseconds(nanos);
            input.query = probe.Context(host.world->Descriptor(), tick);
            CharacterMetricCapture capture;
            input.metrics = &capture;
            const auto callsBefore = probe.calls;
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            const auto advanced = host.world->AdvanceFixedTick(input);
            REQUIRE(capture.snapshot.queries == probe.calls - callsBefore);
            RequireMovementBudget(*host.world, capture);
            if (advanced.HasError())
                UNSCOPED_INFO(advanced.ErrorValue().message);
            REQUIRE(advanced.HasValue());
            return host.world->ControllerLocomotionSnapshot(host.controller).Value();
        }

        TEST_CASE("Jump consumes committed ground once and landing publishes one transition", "[physics][character][airborne]") {
            const auto up = GENERATE(Math::Vec3{0, 1, 0}, Math::Vec3{0, 0, 1});
            auto host = AirWorld(up, up * -9.81F);
            AirProbe probe{up};
            REQUIRE(MoveAir(host, probe, 1).movement.grounded);
            const auto jump = MoveAir(host, probe, 2, true);
            REQUIRE(jump.movement.jumpApplied);
            REQUIRE_FALSE(jump.movement.grounded);
            REQUIRE(jump.movement.groundTransition == CharacterGroundTransition::LeftGround);
            REQUIRE(Math::Dot(jump.movement.gravityVelocityMetersPerSecond, up) == Catch::Approx(5.0F - 9.81F / 60.0F));
            std::uint32_t landed{};
            for (std::uint64_t tick = 3; tick <= 90; ++tick) {
                const auto next = MoveAir(host, probe, tick, tick < 20);
                REQUIRE_FALSE(next.movement.jumpApplied);
                if (next.movement.groundTransition == CharacterGroundTransition::Landed)
                    ++landed;
            }
            REQUIRE(landed == 1);
        }

        TEST_CASE("Ceiling removes forbidden ascent while retaining tangent gravity and subsequent descent",
                  "[physics][character][airborne][ceiling]") {
            auto host = AirWorld({0, 1, 0}, {2, -9.81F, 0});
            AirProbe probe;
            static_cast<void>(MoveAir(host, probe, 1));
            probe.ceiling = 0.1F;
            const auto hit = MoveAir(host, probe, 2, true, 100'000'000);
            REQUIRE((static_cast<std::uint16_t>(hit.movement.collisions) & static_cast<std::uint16_t>(CharacterCollisionFlags::Ceiling)) !=
                    0);
            REQUIRE(hit.movement.gravityVelocityMetersPerSecond.y == 0.0F);
            REQUIRE(hit.movement.gravityVelocityMetersPerSecond.x == Catch::Approx(0.2F));
            REQUIRE_FALSE(hit.movement.grounded);
            probe.floor = false;
            const auto falling = MoveAir(host, probe, 3, true);
            REQUIRE_FALSE(falling.movement.jumpApplied);
            REQUIRE(falling.movement.gravityVelocityMetersPerSecond.y < 0.0F);
        }

        TEST_CASE("Free flight preserves acceleration below intent threshold and supports arbitrary up",
                  "[physics][character][airborne][boundary]") {
            const auto up = GENERATE(Math::Vec3{0, 1, 0}, Math::Vec3{0, 0, 1});
            auto host = AirWorld(up, up * -9.81F);
            AirProbe probe{up, false};
            const auto first = MoveAir(host, probe, 1, true, 1'000'000);
            REQUIRE_FALSE(first.movement.jumpApplied);
            REQUIRE(Math::Dot(first.movement.finalPosition, up) < 0.02F);
            const auto second = MoveAir(host, probe, 2, false, 1'000'000);
            REQUIRE(Math::Dot(second.movement.gravityVelocityMetersPerSecond, up) == Catch::Approx(-0.01962F));
        }

        TEST_CASE("Malformed airborne evidence and shutdown preserve the prior publication", "[physics][character][airborne][lifecycle]") {
            auto host = AirWorld();
            AirProbe probe;
            const auto before = MoveAir(host, probe, 1);
            auto command = Movement(host.controller, 2, 2);
            command.jumpRequested = true;
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            probe.ceiling = 0.04F;
            SECTION("malformed") {
                probe.malformed = true;
            }
            SECTION("shutdown") {
                probe.shutdownWorld = host.world.get();
            }
            auto input = FixedTick(2);
            input.query = probe.Context(host.world->Descriptor(), 2);
            REQUIRE(host.world->AdvanceFixedTick(input).HasError());
            REQUIRE(host.world->PublishedTick().completedTick == 1);
            if (!probe.shutdownWorld)
                REQUIRE(host.world->ControllerLocomotionSnapshot(host.controller).Value().stateRevision == before.stateRevision);
        }

        TEST_CASE("Jump tuning rejects malformed values and zero disables the impulse", "[physics][character][airborne][validation]") {
            auto host = PreparedWorld(1);
            auto descriptor = ControllerDescriptor(host->Descriptor());
            descriptor.jumpSpeedMetersPerSecond =
                GENERATE(-1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
            RequireError(host->CreateController(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor.jumpSpeedMetersPerSecond = 0;
            REQUIRE(host->CreateController(descriptor).HasValue());
        }

        TEST_CASE("Airborne shape changes preserve velocity and malformed transition facts fail validation",
                  "[physics][character][airborne][shape][validation]") {
            const auto up = GENERATE(Math::Vec3{0, 1, 0}, Math::Vec3{0, 0, 1});
            auto host = AirWorld(up, up * -9.81F);
            AirProbe probe{up};
            static_cast<void>(MoveAir(host, probe, 1));
            const auto jumped = MoveAir(host, probe, 2, true);
            auto request = Movement(host.controller, 3, 3);
            request.shapeChange = CharacterShapeChangeRequest{{0.4F, 0.3F}};
            REQUIRE(host.world->QueueMovementCommand(request).HasValue());
            auto input = FixedTick(3);
            input.query = probe.Context(host.world->Descriptor(), 3);
            const auto advanced = host.world->AdvanceFixedTick(input);
            if (advanced.HasError())
                UNSCOPED_INFO(advanced.ErrorValue().message);
            REQUIRE(advanced.HasValue());
            const auto resized = host.world->ControllerLocomotionSnapshot(host.controller).Value();
            REQUIRE_FALSE(resized.movement.grounded);
            REQUIRE(resized.movement.groundTransition == CharacterGroundTransition::None);
            REQUIRE(Math::Dot(resized.movement.gravityVelocityMetersPerSecond, up) ==
                    Catch::Approx(Math::Dot(jumped.movement.gravityVelocityMetersPerSecond, up) - 9.81F / 60.0F));
            auto malformed = resized.movement;
            malformed.groundTransition = static_cast<CharacterGroundTransition>(255);
            REQUIRE(ValidateCharacterMovementResult(malformed, host.world->ControllerDescriptor(host.controller).Value()).HasError());
        }

        TEST_CASE("Airborne gravity rejects displacement overflow before a movement sweep", "[physics][character][airborne][capacity]") {
            auto host = AirWorld({0, 1, 0}, {0, -std::numeric_limits<float>::max(), 0});
            AirProbe probe{{0, 1, 0}, false};
            REQUIRE(host.world->QueueMovementCommand(Movement(host.controller, 1, 1)).HasValue());
            auto input = FixedTick(1);
            input.query = probe.Context(host.world->Descriptor(), 1);
            REQUIRE(host.world->AdvanceFixedTick(input).HasError());
            REQUIRE(host.world->PublishedTick().completedTick == 0);
            REQUIRE(probe.calls == 1);
        }

        TEST_CASE("Airborne teleport resets continuation without replaying the jump", "[physics][character][airborne][teleport]") {
            auto host = AirWorld();
            AirProbe probe;
            static_cast<void>(MoveAir(host, probe, 1));
            static_cast<void>(MoveAir(host, probe, 2, true));
            OverlapProbe overlap;
            const CharacterTeleportRequest request{host.controller, 3, {0, 3, 0}, Math::Quaternion::Identity()};
            REQUIRE(host.world->TeleportController(request, overlap.Context(host.world->Descriptor(), 3)).HasValue());
            REQUIRE(host.world->AdvanceFixedTick(FixedTick(3)).HasValue());
            const auto next = MoveAir(host, probe, 4, true);
            REQUIRE_FALSE(next.movement.jumpApplied);
            REQUIRE(next.movement.gravityVelocityMetersPerSecond.y == Catch::Approx(-9.81F / 60.0F));
        }
    }  // namespace
}  // namespace Horo::Character
