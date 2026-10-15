#pragma once

#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Character::TestDetail {
    /** @brief Supplies unobstructed placement without borrowing or modifying fixture state. */
    constexpr CharacterOverlapProbe UnobstructedPlatformSpace = [](void *, const CharacterOverlapProbeRequest &) noexcept {
        return Result<CharacterOverlapProbeResult>::Success({});
    };

    struct PlatformProbe final {
        CharacterSweepHit ground;
        CharacterPlatformBodyEvidence body;
        bool groundPresent{true};
        bool bodyPresent{true};
        bool fail{};
        bool wall{};
        CharacterWorld *shutdownWorld{};
        std::uint32_t bodyReads{};
        std::uint32_t failOnRead{};

        static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
            const auto &probe = *static_cast<PlatformProbe *>(context);
            CharacterSweepProbeResult result;
            if (probe.groundPresent && request.direction.y < -0.5F && probe.ground.distanceMeters <= request.maximumDistanceMeters) {
                result.hits[0] = probe.ground;
                result.hitCount = 1;
            }
            if (probe.wall && request.direction.x > 0.5F) {
                result.hits[0] = probe.ground;
                result.hits[0].normal = {-1, 0, 0};
                result.hits[0].distanceMeters = 0.05F;
                result.hitCount = 1;
            }
            return Result<CharacterSweepProbeResult>::Success(result);
        }

        static Result<std::optional<CharacterPlatformBodyEvidence>> Body(void *context, Physics::BodyHandle, Physics::ShapeHandle,
                                                                         std::uint64_t) noexcept {
            auto &probe = *static_cast<PlatformProbe *>(context);
            ++probe.bodyReads;
            if (probe.shutdownWorld != nullptr)
                probe.shutdownWorld->Shutdown();
            if (probe.fail || probe.bodyReads == probe.failOnRead)
                return Result<std::optional<CharacterPlatformBodyEvidence>>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
            return Result<std::optional<CharacterPlatformBodyEvidence>>::Success(probe.bodyPresent ? std::optional{probe.body}
                                                                                                   : std::nullopt);
        }

        CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
            return {world.sceneGeneration,
                    world.identity,
                    world.physicsWorld,
                    this,
                    UnobstructedPlatformSpace,
                    world.collisionFilterGeneration,
                    world.originGeneration,
                    tick,
                    world.physicsSnapshotRevision,
                    Sweep,
                    Body};
        }
    };

    struct PlatformScenario final {
        std::unique_ptr<CharacterWorld> world;
        CharacterControllerHandle controller;
        CharacterControllerHandle secondController;
        PlatformProbe probe;

        explicit PlatformScenario(const bool dynamicAllowed = false, const std::uint32_t queries = 64, const bool twoControllers = false,
                                  const CharacterPlatformLeaveVelocityPolicy policy = CharacterPlatformLeaveVelocityPolicy::None,
                                  const bool inheritHeading = false, const Math::Vec3 root = {}) {
            auto settings = Settings().Values();
            settings.work.maximumQueriesPerTick = queries;
            world = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value()).Value();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.gravity = {};
            descriptor.platformLeaveVelocity = policy;
            descriptor.inheritPlatformHeading = inheritHeading;
            descriptor.collisionRootPosition = root;
            descriptor.allowDynamicPlatformAttachment = dynamicAllowed;
            controller = world->CreateController(descriptor).Value();
            if (twoControllers)
                secondController = world->CreateController(descriptor).Value();
            ActivateControllers(twoControllers);
            const Physics::BodyHandle body{world->Descriptor().physicsWorld, {4, 2}};
            const Physics::ShapeHandle shape{world->Descriptor().physicsWorld, {5, 3}};
            probe.ground = {.body = body, .shape = shape, .point = {2, 0, 1}, .normal = {0, 1, 0}, .distanceMeters = 0.02F};
            probe.body = {body, shape, Physics::PhysicsMotionType::Kinematic, {}};
        }

        void ActivateControllers(const bool twoControllers) {
            REQUIRE(world->Activate().HasValue());
            const std::array controllers{controller, secondController};
            for (std::size_t index = 0; index < (twoControllers ? 2U : 1U); ++index) {
                const auto &descriptor = world->Descriptor();
                CharacterPhysicsQueryContext query{descriptor.sceneGeneration,        descriptor.identity,
                                                   descriptor.physicsWorld,           nullptr,
                                                   UnobstructedPlatformSpace,         descriptor.collisionFilterGeneration,
                                                   descriptor.originGeneration,       0,
                                                   descriptor.physicsSnapshotRevision};
                REQUIRE(world->SpawnController(controllers[index], query).HasValue());
            }
        }

        CharacterFixedTickInput QueuedTick(const std::uint64_t tick) {
            REQUIRE(world->QueueMovementCommand(Movement(controller, tick, tick)).HasValue());
            auto input = FixedTick(tick);
            input.query = probe.Context(world->Descriptor(), tick);
            return input;
        }

        Result<void> Tick(const std::uint64_t tick, const bool jump = false,
                          const std::span<const Physics::PhysicsKinematicTargetCommand> targets = {}, const bool detach = false,
                          const Math::Vec3 velocity = {}) {
            auto command = Movement(controller, tick, tick);
            command.jumpRequested = jump;
            command.detachPlatformRequested = detach;
            command.desiredVelocityMetersPerSecond = velocity;
            REQUIRE(world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(tick);
            input.query = probe.Context(world->Descriptor(), tick);
            input.platformTargets = targets;
            return world->AdvanceFixedTick(input);
        }

        CharacterLocomotionSnapshot Snapshot() const {
            return world->ControllerLocomotionSnapshot(controller).Value();
        }
    };

    inline void RequireNear(const Math::Vec3 actual, const Math::Vec3 expected) {
        REQUIRE(actual.x == Catch::Approx(expected.x).margin(1.0e-5F));
        REQUIRE(actual.y == Catch::Approx(expected.y).margin(1.0e-5F));
        REQUIRE(actual.z == Catch::Approx(expected.z).margin(1.0e-5F));
    }

}  // namespace Horo::Character::TestDetail
