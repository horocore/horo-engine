#pragma once

#include "CharacterControllerRegistry.h"
#include "Horo/Physics/CharacterWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Character::TestDetail {
    [[nodiscard]] inline CharacterWorldId WorldId(const std::uint64_t value = 71) {
        const auto result = CharacterWorldId::Create(value);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    [[nodiscard]] inline Physics::PhysicsWorldId PhysicsWorldId(const std::uint64_t value = 81) {
        const auto result = Physics::PhysicsWorldId::Create(value);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    [[nodiscard]] inline CharacterWorldPreparationDescriptor WorldDescriptor() {
        return {61, PhysicsWorldId(), 91, 101};
    }

    [[nodiscard]] inline CharacterWorldSettings Settings(const std::uint32_t maximumControllers = 2,
                                                         const std::uint32_t maximumRecoveryIterations = 8) {
        CharacterWorldSettingsDescriptor descriptor;
        descriptor.capacities.maximumControllers = maximumControllers;
        descriptor.work.maximumRecoveryIterations = maximumRecoveryIterations;
        const auto result = CharacterWorldSettings::Capture(descriptor);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    [[nodiscard]] inline Physics::PhysicsQueryMaterial Material() {
        return {
            Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
            3,
            Physics::PhysicsMaterialSlotId::FromValue(5),
        };
    }

    [[nodiscard]] inline CharacterControllerDescriptor ControllerDescriptor(const CharacterWorldDescriptor &world) {
        CharacterControllerDescriptor descriptor;
        descriptor.sceneGeneration = world.sceneGeneration;
        descriptor.characterWorld = world.identity;
        descriptor.physicsWorld = world.physicsWorld;
        descriptor.collisionProfile = Physics::CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
        descriptor.queryChannel = Physics::PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
        descriptor.defaultMaterial = Material();
        return descriptor;
    }

    using Physics::Test::RequireError;

    [[nodiscard]] inline std::unique_ptr<CharacterWorld> PreparedWorld(const std::uint32_t maximumControllers = 2,
                                                                       const std::uint32_t maximumRecoveryIterations = 8) {
        const auto settings = Settings(maximumControllers, maximumRecoveryIterations);
        auto prepared = CharacterWorld::Prepare(WorldDescriptor(), settings);
        REQUIRE(prepared.HasValue());
        return std::move(prepared).Value();
    }

    struct ActiveWorld final {
        std::unique_ptr<CharacterWorld> world;
        std::array<CharacterControllerHandle, 2> controllers{};
    };

    [[nodiscard]] inline ActiveWorld ActiveWorldWithControllers(const std::uint32_t controllerCount = 1,
                                                                const std::uint32_t maximumRecoveryIterations = 8) {
        REQUIRE(controllerCount <= 2);
        ActiveWorld result{PreparedWorld(controllerCount, maximumRecoveryIterations)};
        const auto descriptor = ControllerDescriptor(result.world->Descriptor());
        for (std::uint32_t index = 0; index < controllerCount; ++index)
            result.controllers[index] = result.world->CreateController(descriptor).Value();
        REQUIRE(result.world->Activate().HasValue());
        return result;
    }

    [[nodiscard]] inline CharacterMovementRequest Movement(const CharacterControllerHandle controller, const std::uint64_t tick,
                                                           const std::uint64_t sequence) {
        return {.controller = controller, .tick = tick, .sequence = sequence};
    }

    [[nodiscard]] inline CharacterFixedTickInput FixedTick(const std::uint64_t tick, const CharacterTickObserver observer = {}) {
        return {.tick = tick,
                .sceneGeneration = WorldDescriptor().sceneGeneration,
                .fixedDelta = Duration::FromNanoseconds(16'666'667),
                .observer = observer};
    }

    struct OverlapProbe final {
        std::uint32_t calls{};
        std::uint32_t overlappingCalls{};
        Math::Vec3 recoveryDisplacement{0, 1, 0};
        bool alwaysOverlapping{};
        std::array<Math::Vec3, CharacterWorldSettingLimits::MaximumRecoveryIterations + 2> positions{};

        static Result<CharacterOverlapProbeResult> Run(void *context, const CharacterOverlapProbeRequest &request) noexcept {
            auto &probe = *static_cast<OverlapProbe *>(context);
            probe.positions[probe.calls] = request.position;
            ++probe.calls;
            if (probe.alwaysOverlapping || probe.calls <= probe.overlappingCalls)
                return Result<CharacterOverlapProbeResult>::Success({1, probe.recoveryDisplacement});
            return Result<CharacterOverlapProbeResult>::Success({});
        }

        [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick = 0) noexcept {
            return {world.sceneGeneration,  world.identity, world.physicsWorld,           this, Run, world.collisionFilterGeneration,
                    world.originGeneration, tick,           world.physicsSnapshotRevision};
        }
    };

    struct SpawnedActiveWorld final {
        std::unique_ptr<CharacterWorld> world;
        CharacterControllerHandle controller;
    };

    [[nodiscard]] inline SpawnedActiveWorld SpawnedActiveWorldWithController(const std::uint32_t maximumRecoveryIterations = 8) {
        auto active = ActiveWorldWithControllers(1, maximumRecoveryIterations);
        const auto controller = active.controllers[0];
        OverlapProbe probe;
        REQUIRE(active.world->SpawnController(controller, probe.Context(active.world->Descriptor())).HasValue());
        return {std::move(active.world), controller};
    }

    struct CommandTrace final {
        std::array<CharacterTickPhase, 3> phases{};
        std::array<CharacterMovementRequest, 2> movements{};
        std::size_t phaseCount{};
        std::size_t movementCount{};

        static void Phase(void *context, const CharacterTickPhase phase, const std::uint64_t) noexcept {
            auto &trace = *static_cast<CommandTrace *>(context);
            trace.phases[trace.phaseCount++] = phase;
        }

        static void Movement(void *context, const CharacterMovementRequest &request) noexcept {
            auto &trace = *static_cast<CommandTrace *>(context);
            trace.movements[trace.movementCount++] = request;
        }

        [[nodiscard]] CharacterTickObserver Observer() noexcept {
            return {.context = this, .phase = Phase, .movement = Movement};
        }
    };

    struct ReentrantPlacementProbe final {
        CharacterWorld *world{};
        std::optional<Result<void>> destroyResult;
        bool shutdown{};

        static Result<CharacterOverlapProbeResult> Run(void *context, const CharacterOverlapProbeRequest &request) noexcept {
            auto &probe = *static_cast<ReentrantPlacementProbe *>(context);
            probe.destroyResult = probe.world->DestroyController(request.controller);
            if (probe.shutdown)
                probe.world->Shutdown();
            return Result<CharacterOverlapProbeResult>::Success({});
        }

        [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &descriptor) noexcept {
            return {descriptor.sceneGeneration,
                    descriptor.identity,
                    descriptor.physicsWorld,
                    this,
                    Run,
                    descriptor.collisionFilterGeneration,
                    descriptor.originGeneration,
                    0,
                    descriptor.physicsSnapshotRevision};
        }
    };
}  // namespace Horo::Character::TestDetail
