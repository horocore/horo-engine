#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] ParticleSystemDescriptor Descriptor(const ParticleCollisionMode mode,
                                                          const ParticleCollisionResponse response = ParticleCollisionResponse::Bounce) {
            auto data = Tests::ValidParticleDescriptorData();
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.maximumParticles = 16;
            data.spawnRate = {};
            data.initialSpeed = {};
            data.lifetimeSeconds = {10.0, 10.0};
            data.collisionMode = mode;
            data.collisionResponse = response;
            return Tests::ParticleDescriptor(std::move(data));
        }

        [[nodiscard]] CpuParticleSimulatorCreateInfo Info() {
            const auto scope = VfxIdentityScope::Create(77).Value();
            return {.buffer = MakeVfxIdentity<ParticleBufferIdentityTag>(scope, 1, 1).Value(),
                    .activation = MakeVfxIdentity<EffectSystemIdentityTag>(scope, 2, 1).Value(),
                    .effectSeed = 42,
                    .maximumBurstParticles = 16};
        }

        struct PhysicsSnapshot final {
            std::uint32_t calls{};
            std::uint64_t tick{};
            std::uint64_t scene{};
            std::uint64_t snapshot{};
            bool fail{};
            bool invalid{};
        };

        Result<CpuParticleCollisionHit> Probe(void *context, const CpuParticleCollisionQueryRequest &request) noexcept {
            auto &snapshot = *static_cast<PhysicsSnapshot *>(context);
            ++snapshot.calls;
            snapshot.tick = request.tick;
            snapshot.scene = request.sceneGeneration;
            snapshot.snapshot = request.snapshotGeneration;
            if (snapshot.fail)
                return Result<CpuParticleCollisionHit>::Failure(MakeError(VfxErrors::ParticleStageContractViolation));
            return Result<CpuParticleCollisionHit>::Success({.hit = true,
                                                             .position = {request.position.x, request.position.y, 0.25F},
                                                             .normal = snapshot.invalid ? Math::Vec3{} : Math::Vec3{0.0F, 0.0F, -2.0F},
                                                             .distance = 0.25F,
                                                             .restitution = 0.5F,
                                                             .stableTarget = 12,
                                                             .stableFeature = 3});
        }

        [[nodiscard]] CpuParticleSimulator Create(const ParticleCollisionMode mode, const ParticleCollisionResponse response,
                                                  const CpuParticleSimulatorCreateInfo &info) {
            auto result = CpuParticleSimulator::Create(Descriptor(mode, response), info);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("CPU collision modes apply descriptor bounce and die with deterministic snapshots", "[unit][vfx][particle-collision]") {
        const std::array forces{CpuParticleForceModule{.vector = {1.0F, 0.0F, 2.0F}}};
        const std::array planes{CpuParticlePlane{.point = {0.0F, 0.0F, 0.25F}, .normal = {0.0F, 0.0F, -2.0F}, .restitution = 0.5F}};
        const std::array samples{CpuParticleDepthSample{.covered = true, .depth = 0.25F, .normal = {0.0F, 0.0F, -2.0F}}};
        for (const auto mode : {ParticleCollisionMode::Planes, ParticleCollisionMode::SceneDepth, ParticleCollisionMode::PhysicsWorld}) {
            for (const auto response : {ParticleCollisionResponse::Bounce, ParticleCollisionResponse::Die}) {
                CAPTURE(mode, response);
                PhysicsSnapshot physics{};
                auto info = Info();
                info.forces = forces;
                info.planes = planes;
                info.sceneDepth = {.width = 1, .height = 1, .samples = samples, .restitution = 0.5F};
                info.physicsWorld = {.context = &physics, .probe = Probe, .sceneGeneration = 9, .snapshotGeneration = 4};
                auto first = Create(mode, response, info);
                auto second = Create(mode, response, info);
                const auto left = first.Advance({.deltaSeconds = 0.5F, .burstCount = 4, .tick = 0});
                const auto right = second.Advance({.deltaSeconds = 0.5F, .burstCount = 4, .tick = 0});
                REQUIRE(left.HasValue());
                REQUIRE(right.HasValue());
                CHECK(left.Value() == right.Value());
                CHECK(left.Value().collisions == 4);
                CHECK(left.Value().killed == (response == ParticleCollisionResponse::Die ? 4 : 0));
                const auto a = first.Extract().Value();
                const auto b = second.Extract().Value();
                for (std::size_t index = 0; index < a.positionX.size(); ++index) {
                    CHECK(a.positionX[index] == b.positionX[index]);
                    CHECK(a.positionY[index] == b.positionY[index]);
                    CHECK(a.positionZ[index] == b.positionZ[index]);
                    CHECK(a.velocityX[index] == 0.5F);  // Restitution preserves tangent velocity.
                    CHECK(a.velocityZ[index] == -0.5F);
                    CHECK(a.velocityZ[index] == b.velocityZ[index]);
                    CHECK(Math::NearlyEqual(a.positionZ[index], 0.2499F));
                    CHECK(first.HandleAtDenseIndex(static_cast<std::uint32_t>(index)).Value().particle ==
                          second.HandleAtDenseIndex(static_cast<std::uint32_t>(index)).Value().particle);
                }
                if (mode == ParticleCollisionMode::PhysicsWorld) {
                    CHECK(physics.calls == 8);
                    CHECK(physics.tick == 0);
                    CHECK(physics.scene == 9);
                    CHECK(physics.snapshot == 4);
                } else {
                    CHECK(physics.calls == 0);
                }
            }
        }
    }

    TEST_CASE("CPU seeded collision replay stays identical across multiple ticks", "[unit][vfx][particle-collision]") {
        const std::array forces{CpuParticleForceModule{.vector = {0.0F, 0.0F, 2.0F}},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = 0.1F, .randomChannel = 19}};
        const std::array planes{CpuParticlePlane{.point = {0.0F, 0.0F, 0.25F}, .normal = {0.0F, 0.0F, -1.0F}}};
        const std::array samples{CpuParticleDepthSample{.covered = true, .depth = 0.25F}};
        for (const auto mode : {ParticleCollisionMode::Planes, ParticleCollisionMode::SceneDepth, ParticleCollisionMode::PhysicsWorld}) {
            PhysicsSnapshot physics{};
            auto info = Info();
            info.forces = forces;
            info.planes = planes;
            info.sceneDepth = {.width = 1, .height = 1, .samples = samples};
            info.physicsWorld = {.context = &physics, .probe = Probe};
            auto first = Create(mode, ParticleCollisionResponse::Bounce, info);
            auto second = Create(mode, ParticleCollisionResponse::Bounce, info);
            for (std::uint64_t tick = 0; tick < 8; ++tick) {
                const auto left = first.Advance({.deltaSeconds = 0.25F, .burstCount = 1, .tick = tick});
                const auto right = second.Advance({.deltaSeconds = 0.25F, .burstCount = 1, .tick = tick});
                REQUIRE(left.HasValue());
                REQUIRE(right.HasValue());
                CHECK(left.Value() == right.Value());
                const auto a = first.Extract().Value();
                const auto b = second.Extract().Value();
                for (std::size_t index = 0; index < a.positionX.size(); ++index) {
                    CHECK(a.positionX[index] == b.positionX[index]);
                    CHECK(a.positionY[index] == b.positionY[index]);
                    CHECK(a.positionZ[index] == b.positionZ[index]);
                    CHECK(a.velocityX[index] == b.velocityX[index]);
                    CHECK(a.velocityY[index] == b.velocityY[index]);
                    CHECK(a.velocityZ[index] == b.velocityZ[index]);
                }
            }
            CHECK(first.Statistics().collisions > 0);
        }
    }

    TEST_CASE("CPU collision composes without Physics and required missing sources reject admission", "[unit][vfx][particle-collision]") {
        const std::array forces{CpuParticleForceModule{.vector = {0.0F, -2.0F, 0.0F}}};
        const std::array planes{CpuParticlePlane{}};
        for (const auto mode : {ParticleCollisionMode::None, ParticleCollisionMode::Planes, ParticleCollisionMode::SceneDepth,
                                ParticleCollisionMode::PhysicsWorld}) {
            auto info = Info();
            info.forces = forces;
            info.planes = planes;
            auto simulator = Create(mode, ParticleCollisionResponse::Bounce, info);
            const auto result = simulator.Advance({.deltaSeconds = 0.5F, .burstCount = 1, .tick = 0});
            REQUIRE(result.HasValue());
            CHECK(result.Value().collisions == (mode == ParticleCollisionMode::Planes ? 1 : 0));
            CHECK(result.Value().active == 1);
        }
        for (const auto mode : {ParticleCollisionMode::SceneDepth, ParticleCollisionMode::PhysicsWorld}) {
            auto info = Info();
            info.sceneDepth.required = true;
            info.physicsWorld.required = true;
            const auto required = CpuParticleSimulator::Create(Descriptor(mode), info);
            REQUIRE(required.HasError());
            CHECK(required.ErrorValue().code.Value() == VfxErrors::ParticleCollisionQueryUnavailable.code.Value());
            info.sceneDepth.required = false;
            info.physicsWorld.required = false;
            info.requiredGameplay = true;
            CHECK(CpuParticleSimulator::Create(Descriptor(mode), info).HasError());
        }
    }

    TEST_CASE("CPU scene depth owns its samples and skips uncovered or out-of-view motion", "[unit][vfx][particle-collision]") {
        std::array samples{CpuParticleDepthSample{.covered = true, .depth = 0.25F}};
        const std::array forces{CpuParticleForceModule{.vector = {0.0F, 0.0F, 2.0F}}};
        auto info = Info();
        info.forces = forces;
        info.sceneDepth = {.width = 1, .height = 1, .samples = samples};
        auto frozen = Create(ParticleCollisionMode::SceneDepth, ParticleCollisionResponse::Bounce, info);
        samples[0].covered = false;
        auto uncovered = Create(ParticleCollisionMode::SceneDepth, ParticleCollisionResponse::Bounce, info);
        CHECK(frozen.Advance({.deltaSeconds = 0.5F, .burstCount = 1}).Value().collisions == 1);
        CHECK(uncovered.Advance({.deltaSeconds = 0.5F, .burstCount = 1}).Value().collisions == 0);
        samples[0].covered = true;
        const std::array offscreen{CpuParticleForceModule{.vector = {8.0F, 0.0F, 2.0F}}};
        info.forces = offscreen;
        auto outside = Create(ParticleCollisionMode::SceneDepth, ParticleCollisionResponse::Bounce, info);
        CHECK(outside.Advance({.deltaSeconds = 0.5F, .burstCount = 1}).Value().collisions == 0);
        info.forces = forces;
        info.sceneDepth.width = 2;
        CHECK(CpuParticleSimulator::Create(Descriptor(ParticleCollisionMode::SceneDepth), info).HasError());
        info.sceneDepth.width = 1;
        samples[0].depth = std::numeric_limits<float>::quiet_NaN();
        CHECK(CpuParticleSimulator::Create(Descriptor(ParticleCollisionMode::SceneDepth), info).HasError());
        samples[0].depth = 0.25F;
        info.sceneDepth.worldToClip = {};
        CHECK(CpuParticleSimulator::Create(Descriptor(ParticleCollisionMode::SceneDepth), info).HasError());
    }

    TEST_CASE("CPU collision query failures discard candidate motion and preserve error cause", "[unit][vfx][particle-collision]") {
        PhysicsSnapshot physics{};
        auto info = Info();
        info.physicsWorld = {.context = &physics, .probe = Probe};
        auto simulator = Create(ParticleCollisionMode::PhysicsWorld, ParticleCollisionResponse::Bounce, info);
        REQUIRE(simulator.Advance({.burstCount = 1, .tick = 0}).HasValue());
        const auto before = simulator.Extract().Value().positionZ[0];
        const auto generation = simulator.Statistics().committedGeneration;
        physics.fail = true;
        const auto failed = simulator.Advance({.deltaSeconds = 0.5F, .burstCount = 1, .tick = 1});
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == VfxErrors::ParticleCollisionQueryFailed.code.Value());
        CHECK(simulator.Statistics().committedGeneration == generation);
        CHECK(simulator.Extract().Value().positionZ[0] == before);
        physics.fail = false;
        physics.invalid = true;
        CHECK(simulator.Advance({.tick = 1}).HasError());
        CHECK(simulator.Statistics().committedGeneration == generation);
        physics.invalid = false;
        REQUIRE(simulator.Advance({.tick = 1}).HasValue());
    }

    TEST_CASE("CPU planes ignore stationary contacts and resolve equal-time ties in declared order", "[unit][vfx][particle-collision]") {
        const std::array planes{CpuParticlePlane{.restitution = 0.0F}, CpuParticlePlane{.restitution = 1.0F}};
        auto info = Info();
        info.planes = planes;
        auto stationary = Create(ParticleCollisionMode::Planes, ParticleCollisionResponse::Bounce, info);
        CHECK(stationary.Advance({.burstCount = 1}).Value().collisions == 0);
        const std::array forces{CpuParticleForceModule{.vector = {1.0F, -2.0F, 0.0F}}};
        info.forces = forces;
        auto moving = Create(ParticleCollisionMode::Planes, ParticleCollisionResponse::Bounce, info);
        REQUIRE(moving.Advance({.deltaSeconds = 0.5F, .burstCount = 1}).HasValue());
        const auto view = moving.Extract().Value();
        CHECK(view.velocityX[0] == 0.5F);
        CHECK(view.velocityY[0] == 0.0F);
    }
}  // namespace Horo::Vfx
