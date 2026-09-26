#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] ParticleSystemDescriptor Descriptor(const float lifetime = 4.0F) {
            auto data = Tests::ValidParticleDescriptorData();
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.maximumParticles = 4;
            data.shape = ParticleEmitterShape::Point;
            data.spawnRate = {0.0, 0.0};
            data.initialSpeed = {0.0, 0.0};
            data.initialSize = {2.0, 2.0};
            data.initialOpacity = {0.8, 0.8};
            data.lifetimeSeconds = {lifetime, lifetime};
            data.collisionMode = ParticleCollisionMode::None;
            return Tests::ParticleDescriptor(std::move(data));
        }

        [[nodiscard]] CpuParticleSimulatorCreateInfo Info(const std::uint32_t slot = 1) {
            const auto scope = VfxIdentityScope::Create(96).Value();
            return {.buffer = MakeVfxIdentity<ParticleBufferIdentityTag>(scope, slot, 1).Value(),
                    .activation = MakeVfxIdentity<EffectSystemIdentityTag>(scope, 3, 1).Value(),
                    .effectSeed = 27,
                    .maximumBurstParticles = 4};
        }

        template <typename T> [[nodiscard]] bool IsError(const Result<T> &result, const ErrorCodeDescriptor &code) {
            return result.HasError() && result.ErrorValue().code.Value() == code.code.Value();
        }

        constexpr std::array Channels{CpuParticlePayloadChannel{.channel = 1,
                                                                .classification = CpuParticlePayloadClass::GameplayInput,
                                                                .customFloatStream = 0,
                                                                .minimum = 0.0F,
                                                                .maximum = 1.0F},
                                      CpuParticlePayloadChannel{.channel = 2,
                                                                .classification = CpuParticlePayloadClass::GameplayOutput,
                                                                .customFloatStream = 1,
                                                                .minimum = 0.0F,
                                                                .maximum = 2.0F},
                                      CpuParticlePayloadChannel{.channel = 3,
                                                                .classification = CpuParticlePayloadClass::RenderOnly,
                                                                .customFloatStream = 2,
                                                                .minimum = 0.0F,
                                                                .maximum = 1.0F},
                                      CpuParticlePayloadChannel{.channel = 4,
                                                                .classification = CpuParticlePayloadClass::SimulationInternal,
                                                                .customFloatStream = 3,
                                                                .minimum = 0.0F,
                                                                .maximum = 1.0F}};
        constexpr std::array Modules{CpuParticlePayloadModule{.readChannel = 1, .writeChannel = 2, .scale = 2.0F}};

        struct ReentrantProbe final {
            CpuParticleSimulator *simulator{};
            bool denied{};
            bool diagnosed{};
        };

        Result<void> TryReentrantWrite(void *context, const CpuParticleStage stage) noexcept {
            if (stage == CpuParticleStage::Integrate) {
                auto &probe = *static_cast<ReentrantProbe *>(context);
                const auto result = probe.simulator->SubmitGameplayInput(1, 0.9F);
                probe.denied = IsError(result, VfxErrors::ParticleStageContractViolation);
                probe.diagnosed = result.HasError() && !result.ErrorValue().diagnostics.empty();
            }
            return Result<void>::Success();
        }

        struct FailureProbe final {
            bool fail{};
        };

        Result<void> FailKill(void *context, const CpuParticleStage stage) noexcept {
            if (stage == CpuParticleStage::Kill && static_cast<FailureProbe *>(context)->fail)
                return Result<void>::Failure(MakeError(VfxErrors::ParticleStageContractViolation));
            return Result<void>::Success();
        }
    }  // namespace

    TEST_CASE("Over-life curves interpolate normalized age and preserve initial opacity", "[unit][vfx][over-life]") {
        const std::array size{CpuParticleCurveKey{0.0F, 1.0F}, CpuParticleCurveKey{1.0F, 2.0F}};
        const std::array opacity{CpuParticleCurveKey{0.0F, 1.0F}, CpuParticleCurveKey{1.0F, 0.0F}};
        const std::array color{CpuParticleColorKey{0.0F, {1.0F, 0.0F, 0.0F, 1.0F}}, CpuParticleColorKey{1.0F, {0.0F, 0.0F, 1.0F, 0.5F}}};
        auto info = Info();
        info.sizeOverLife = size;
        info.opacityOverLife = opacity;
        info.colorOverLife = color;
        auto created = CpuParticleSimulator::Create(Descriptor(), info);
        REQUIRE(created.HasValue());
        auto simulator = std::move(created).Value();
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .burstCount = 1, .tick = 0}).HasValue());
        const auto first = simulator.Extract().Value();
        REQUIRE(first.sizeX.size() == 1);
        CHECK(first.sizeX[0] == Catch::Approx(2.5F));
        CHECK(first.sizeY[0] == Catch::Approx(2.5F));
        CHECK(first.packedColor[0] == 0xBF004086U);
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .tick = 1}).HasValue());
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .tick = 2}).HasValue());
        const auto third = simulator.Extract().Value();
        CHECK(third.sizeX[0] == Catch::Approx(3.5F));
        CHECK(third.packedColor[0] == 0x4000BF20U);
    }

    TEST_CASE("Compiled gameplay payload module respects read write and extract boundaries", "[unit][vfx][payload]") {
        auto info = Info(2);
        info.payloadChannels = Channels;
        info.payloadModules = Modules;
        auto created = CpuParticleSimulator::Create(Descriptor(), info);
        REQUIRE(created.HasValue());
        auto simulator = std::move(created).Value();
        REQUIRE(simulator.SubmitGameplayInput(1, 0.75F).HasValue());
        CHECK(IsError(simulator.SubmitGameplayInput(2, 1.0F), VfxErrors::ParticleGameplayAccessDenied));
        CHECK(IsError(simulator.SubmitGameplayInput(3, 0.5F), VfxErrors::ParticleGameplayAccessDenied));
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .burstCount = 1, .tick = 0}).HasValue());
        const auto view = simulator.Extract().Value();
        REQUIRE(view.customFloatStreamCount == 4);
        CHECK(view.customFloats[0].empty());
        CHECK(view.customFloats[1].empty());
        CHECK(view.customFloats[2].size() == 1);
        CHECK(view.customFloats[3].empty());
        const auto output = simulator.ReadGameplayOutput(2, 0, view.committedGeneration);
        REQUIRE(output.HasValue());
        CHECK(output.Value() == Catch::Approx(1.5F));
        CHECK(IsError(simulator.ReadGameplayOutput(1, 0, view.committedGeneration), VfxErrors::ParticleGameplayAccessDenied));
        CHECK(IsError(simulator.ReadGameplayOutput(3, 0, view.committedGeneration), VfxErrors::ParticleGameplayAccessDenied));
        CHECK(IsError(simulator.ReadGameplayOutput(2, 0, view.committedGeneration - 1), VfxErrors::ParticleGenerationStale));
    }

    TEST_CASE("Payload module contracts fail preparation with typed diagnostics", "[unit][vfx][payload]") {
        const auto descriptor = Descriptor();
        auto rejects = [&descriptor](const std::span<const CpuParticlePayloadChannel> channels,
                                     const std::span<const CpuParticlePayloadModule> modules, const ErrorCodeDescriptor &code) {
            auto info = Info(3);
            info.payloadChannels = channels;
            info.payloadModules = modules;
            const auto result = CpuParticleSimulator::Create(descriptor, info);
            REQUIRE(IsError(result, code));
            CHECK(!result.ErrorValue().diagnostics.empty());
            CHECK(result.ErrorValue().diagnostics[0].path.starts_with("payloadModules["));
        };
        const std::array wrongStage{CpuParticlePayloadModule{.stage = CpuParticleStage::Forces, .readChannel = 1, .writeChannel = 2}};
        const std::array privateRead{CpuParticlePayloadModule{.readChannel = 4, .writeChannel = 2}};
        const std::array renderWrite{CpuParticlePayloadModule{.readChannel = 1, .writeChannel = 3}};
        const std::array unknownRead{CpuParticlePayloadModule{.readChannel = 99, .writeChannel = 2}};
        const std::array duplicateWriters{Modules[0], Modules[0]};
        const std::array outOfRange{CpuParticlePayloadModule{.readChannel = 1, .writeChannel = 2, .scale = 3.0F}};
        rejects(Channels, wrongStage, VfxErrors::ParticleStageContractViolation);
        rejects(Channels, privateRead, VfxErrors::ParticleGameplayAccessDenied);
        rejects(Channels, renderWrite, VfxErrors::ParticleGameplayAccessDenied);
        rejects(Channels, unknownRead, VfxErrors::ParticlePayloadSchemaMismatch);
        rejects(Channels, duplicateWriters, VfxErrors::ParticleStageContractViolation);
        rejects(Channels, outOfRange, VfxErrors::ParticlePayloadSchemaMismatch);

        auto aliased = Channels;
        aliased[1].customFloatStream = aliased[0].customFloatStream;
        auto info = Info(4);
        info.payloadChannels = aliased;
        CHECK(IsError(CpuParticleSimulator::Create(descriptor, info), VfxErrors::ParticlePayloadSchemaMismatch));
    }

    TEST_CASE("Invalid over-life key domains and duplicate ages fail preparation", "[unit][vfx][over-life]") {
        const auto descriptor = Descriptor();
        auto info = Info(5);
        const std::array duplicateSize{CpuParticleCurveKey{0.5F, 1.0F}, CpuParticleCurveKey{0.5F, 2.0F}};
        info.sizeOverLife = duplicateSize;
        CHECK(IsError(CpuParticleSimulator::Create(descriptor, info), VfxErrors::ParticleSimulationDescriptorInvalid));
        const std::array negativeSize{CpuParticleCurveKey{0.0F, -1.0F}};
        info.sizeOverLife = negativeSize;
        CHECK(IsError(CpuParticleSimulator::Create(descriptor, info), VfxErrors::ParticleSimulationDescriptorInvalid));
        info.sizeOverLife = {};
        const std::array highOpacity{CpuParticleCurveKey{0.0F, 1.1F}};
        info.opacityOverLife = highOpacity;
        CHECK(IsError(CpuParticleSimulator::Create(descriptor, info), VfxErrors::ParticleSimulationDescriptorInvalid));
        info.opacityOverLife = {};
        const std::array highColor{CpuParticleColorKey{0.0F, {1.0F, 1.2F, 0.0F, 1.0F}}};
        info.colorOverLife = highColor;
        CHECK(IsError(CpuParticleSimulator::Create(descriptor, info), VfxErrors::ParticleSimulationDescriptorInvalid));
    }

    TEST_CASE("Active steps freeze gameplay input and surface a stage diagnostic", "[unit][vfx][payload]") {
        ReentrantProbe probe{};
        auto info = Info(6);
        info.payloadChannels = Channels;
        info.payloadModules = Modules;
        info.stageObserver = TryReentrantWrite;
        info.stageObserverContext = &probe;
        auto created = CpuParticleSimulator::Create(Descriptor(), info);
        REQUIRE(created.HasValue());
        auto simulator = std::move(created).Value();
        probe.simulator = &simulator;
        REQUIRE(simulator.SubmitGameplayInput(1, 0.4F).HasValue());
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .burstCount = 1, .tick = 0}).HasValue());
        CHECK(probe.denied);
        CHECK(probe.diagnosed);
        const auto output = simulator.ReadGameplayOutput(2, 0, 1);
        REQUIRE(output.HasValue());
        CHECK(output.Value() == Catch::Approx(0.8F));
        CHECK(simulator.SubmitGameplayInput(1, 0.5F).HasValue());
    }

    TEST_CASE("Payload stage failure retains the prior committed output", "[unit][vfx][payload]") {
        FailureProbe probe{};
        auto info = Info(8);
        info.payloadChannels = Channels;
        info.payloadModules = Modules;
        info.stageObserver = FailKill;
        info.stageObserverContext = &probe;
        auto created = CpuParticleSimulator::Create(Descriptor(), info);
        REQUIRE(created.HasValue());
        auto simulator = std::move(created).Value();
        REQUIRE(simulator.SubmitGameplayInput(1, 0.4F).HasValue());
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .burstCount = 1, .tick = 0}).HasValue());
        REQUIRE(simulator.SubmitGameplayInput(1, 1.0F).HasValue());
        probe.fail = true;
        CHECK(IsError(simulator.Advance({.deltaSeconds = 1.0F, .tick = 1}), VfxErrors::ParticleStageContractViolation));
        CHECK(simulator.Statistics().committedGeneration == 1);
        const auto prior = simulator.ReadGameplayOutput(2, 0, 1);
        REQUIRE(prior.HasValue());
        CHECK(prior.Value() == Catch::Approx(0.8F));
        probe.fail = false;
        REQUIRE(simulator.Advance({.deltaSeconds = 1.0F, .tick = 1}).HasValue());
        const auto next = simulator.ReadGameplayOutput(2, 0, 2);
        REQUIRE(next.HasValue());
        CHECK(next.Value() == Catch::Approx(2.0F));
    }

    TEST_CASE("Over-life and payload replay is repeatable for identical descriptors", "[unit][vfx][payload]") {
        const std::array size{CpuParticleCurveKey{0.0F, 1.0F}, CpuParticleCurveKey{1.0F, 2.0F}};
        const std::array color{CpuParticleColorKey{0.0F, {1.0F, 0.0F, 0.0F, 1.0F}}, CpuParticleColorKey{1.0F, {0.0F, 0.0F, 1.0F, 0.5F}}};
        auto leftInfo = Info(9);
        leftInfo.payloadChannels = Channels;
        leftInfo.payloadModules = Modules;
        leftInfo.sizeOverLife = size;
        leftInfo.colorOverLife = color;
        auto rightInfo = leftInfo;
        rightInfo.buffer = Info(10).buffer;
        const auto descriptor = Descriptor(10.0F);
        auto leftCreated = CpuParticleSimulator::Create(descriptor, leftInfo);
        auto rightCreated = CpuParticleSimulator::Create(descriptor, rightInfo);
        REQUIRE(leftCreated.HasValue());
        REQUIRE(rightCreated.HasValue());
        auto left = std::move(leftCreated).Value();
        auto right = std::move(rightCreated).Value();
        REQUIRE(left.SubmitGameplayInput(1, 0.6F).HasValue());
        REQUIRE(right.SubmitGameplayInput(1, 0.6F).HasValue());
        for (std::uint64_t tick = 0; tick < 20; ++tick) {
            const CpuParticleSimulationStep step{.deltaSeconds = 0.25F, .burstCount = tick == 0 ? 2U : 0U, .tick = tick};
            REQUIRE(left.Advance(step).HasValue());
            REQUIRE(right.Advance(step).HasValue());
            const auto a = left.Extract().Value();
            const auto b = right.Extract().Value();
            REQUIRE(a.sizeX.size() == b.sizeX.size());
            for (std::size_t dense = 0; dense < a.sizeX.size(); ++dense) {
                CHECK(a.sizeX[dense] == b.sizeX[dense]);
                CHECK(a.packedColor[dense] == b.packedColor[dense]);
                const auto outputA = left.ReadGameplayOutput(2, static_cast<std::uint32_t>(dense), a.committedGeneration);
                const auto outputB = right.ReadGameplayOutput(2, static_cast<std::uint32_t>(dense), b.committedGeneration);
                REQUIRE(outputA.HasValue());
                REQUIRE(outputB.HasValue());
                CHECK(outputA.Value() == outputB.Value());
            }
        }
    }

    TEST_CASE("Curves and payload modules allocate nothing during steady-state steps", "[unit][vfx][payload]") {
        const std::array size{CpuParticleCurveKey{0.0F, 1.0F}, CpuParticleCurveKey{1.0F, 2.0F}};
        const std::array opacity{CpuParticleCurveKey{0.0F, 1.0F}, CpuParticleCurveKey{1.0F, 0.0F}};
        const std::array color{CpuParticleColorKey{0.0F, {1.0F, 0.0F, 0.0F, 1.0F}}, CpuParticleColorKey{1.0F, {0.0F, 0.0F, 1.0F, 0.5F}}};
        auto info = Info(7);
        info.payloadChannels = Channels;
        info.payloadModules = Modules;
        info.sizeOverLife = size;
        info.opacityOverLife = opacity;
        info.colorOverLife = color;
        auto created = CpuParticleSimulator::Create(Descriptor(1'000.0F), info);
        REQUIRE(created.HasValue());
        auto simulator = std::move(created).Value();
        REQUIRE(simulator.SubmitGameplayInput(1, 0.5F).HasValue());
        REQUIRE(simulator.Advance({.deltaSeconds = 0.01F, .burstCount = 4, .tick = 0}).HasValue());
        const std::size_t before = ::Horo::Tests::AllocationProbe::Count();
        bool succeeded = true;
        for (std::uint64_t tick = 1; tick <= 100; ++tick)
            succeeded = succeeded && simulator.Advance({.deltaSeconds = 0.01F, .tick = tick}).HasValue();
        const std::size_t after = ::Horo::Tests::AllocationProbe::Count();
        CHECK(succeeded);
        CHECK(after == before);
    }
}  // namespace Horo::Vfx
