#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <utility>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] ParticleSystemDescriptor ForceDescriptor(const std::uint32_t capacity = 16) {
            auto data = Tests::ValidParticleDescriptorData();
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.shape = ParticleEmitterShape::Point;
            data.maximumParticles = capacity;
            data.spawnRate = {0.0, 0.0};
            data.initialSpeed = {0.0, 0.0};
            data.initialSize = {1.0, 1.0};
            data.initialOpacity = {1.0, 1.0};
            data.lifetimeSeconds = {20.0, 20.0};
            return Tests::ParticleDescriptor(std::move(data));
        }

        [[nodiscard]] CpuParticleSimulatorCreateInfo ForceInfo(const std::span<const CpuParticleForceModule> forces,
                                                               const std::uint32_t slot = 1) {
            const auto scope = VfxIdentityScope::Create(93).Value();
            return {.buffer = MakeVfxIdentity<ParticleBufferIdentityTag>(scope, slot, 1).Value(),
                    .activation = MakeVfxIdentity<EffectSystemIdentityTag>(scope, 4, 1).Value(),
                    .effectSeed = 0x123456789ABCDEF0ULL,
                    .maximumBurstParticles = 16,
                    .forces = forces};
        }

        [[nodiscard]] CpuParticleSimulator MakeSimulator(const ParticleSystemDescriptor &descriptor,
                                                         const std::span<const CpuParticleForceModule> forces,
                                                         const std::uint32_t slot = 1) {
            auto created = CpuParticleSimulator::Create(descriptor, ForceInfo(forces, slot));
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }
    }  // namespace

    TEST_CASE("Force modules apply in descriptor order before integration", "[unit][vfx][force]") {
        const std::array forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0.0F, -1.0F, 0.0F}},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Wind, .vector = {1.0F, 0.0F, 0.0F}, .strength = 2.0F},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Attraction,
                                                       .center = {0.0F, 2.0F, 0.0F},
                                                       .strength = 4.0F,
                                                       .falloff = 0.5F}};
        auto simulator = MakeSimulator(ForceDescriptor(), forces);
        REQUIRE(simulator.Advance({.deltaSeconds = 0.25F, .burstCount = 1, .tick = 0}).HasValue());
        const auto view = simulator.Extract().Value();
        REQUIRE(view.positionX.size() == 1);
        CHECK(view.velocityX[0] == Catch::Approx(0.5F));
        CHECK(view.velocityY[0] == Catch::Approx(0.25F));
        CHECK(view.positionX[0] == Catch::Approx(0.125F));
        CHECK(view.positionY[0] == Catch::Approx(0.0625F));
        CHECK(view.velocityZ[0] == 0.0F);
    }

    TEST_CASE("Attraction at its center remains finite and has no direction", "[unit][vfx][force]") {
        const std::array forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Attraction, .strength = 20.0F}};
        auto simulator = MakeSimulator(ForceDescriptor(), forces);
        REQUIRE(simulator.Advance({.deltaSeconds = 0.25F, .burstCount = 1, .tick = 0}).HasValue());
        const auto view = simulator.Extract().Value();
        CHECK(view.velocityX[0] == 0.0F);
        CHECK(view.velocityY[0] == 0.0F);
        CHECK(view.velocityZ[0] == 0.0F);
    }

    TEST_CASE("Noise replay is stable across identical instances and independent semantic channels", "[unit][vfx][force]") {
        const std::array forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = 0.5F, .randomChannel = 19},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0.0F, -1.0F, 0.0F}},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = 0.25F, .randomChannel = 20}};
        const auto descriptor = ForceDescriptor();
        auto left = MakeSimulator(descriptor, forces, 2);
        auto right = MakeSimulator(descriptor, forces, 3);
        for (std::uint64_t tick = 0; tick < 120; ++tick) {
            const CpuParticleSimulationStep step{.deltaSeconds = 1.0F / 60.0F, .burstCount = tick == 0 ? 16U : 0U, .tick = tick};
            REQUIRE(left.Advance(step).HasValue());
            REQUIRE(right.Advance(step).HasValue());
            const auto a = left.Extract().Value();
            const auto b = right.Extract().Value();
            REQUIRE(a.positionX.size() == b.positionX.size());
            if (tick == 0) {
                // Fixed-seed/channel golden: float fields use a cross-platform absolute tolerance.
                REQUIRE(a.positionX.size() == 16);
                CHECK(a.velocityX[0] == Catch::Approx(-0.010308341F).margin(0.000001F));
                CHECK(a.velocityY[0] == Catch::Approx(-0.016924892F).margin(0.000001F));
                CHECK(a.velocityZ[0] == Catch::Approx(0.009331253F).margin(0.000001F));
            }
            for (std::size_t i = 0; i < a.positionX.size(); ++i) {
                CHECK(a.positionX[i] == b.positionX[i]);
                CHECK(a.positionY[i] == b.positionY[i]);
                CHECK(a.positionZ[i] == b.positionZ[i]);
                CHECK(a.velocityX[i] == b.velocityX[i]);
                CHECK(a.velocityY[i] == b.velocityY[i]);
                CHECK(a.velocityZ[i] == b.velocityZ[i]);
                CHECK(a.packedColor[i] == b.packedColor[i]);
            }
        }
    }

    TEST_CASE("Invalid force descriptors fail before simulator publication", "[unit][vfx][force]") {
        const auto descriptor = ForceDescriptor();
        auto rejects = [&descriptor](const std::span<const CpuParticleForceModule> forces) {
            auto created = CpuParticleSimulator::Create(descriptor, ForceInfo(forces));
            CHECK(created.HasError());
            if (created.HasError())
                CHECK(created.ErrorValue().code.Value() == VfxErrors::ParticleSimulationDescriptorInvalid.code.Value());
        };
        const std::array zeroChannel{CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .randomChannel = 0}};
        const std::array reservedChannel{CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .randomChannel = 9}};
        const std::array duplicateChannels{CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .randomChannel = 16},
                                           CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .randomChannel = 16}};
        const std::array overflowingGravity{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity,
                                                                   .vector = {std::numeric_limits<float>::max(), 0.0F, 0.0F},
                                                                   .strength = 2.0F}};
        rejects(zeroChannel);
        rejects(reservedChannel);
        rejects(duplicateChannels);
        rejects(overflowingGravity);
    }
}  // namespace Horo::Vfx
