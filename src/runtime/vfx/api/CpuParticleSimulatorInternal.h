#pragma once

#include "Horo/Vfx/CpuParticleSimulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace Horo::Vfx::Detail {
    struct CpuParticleBirthPlan final {
        std::uint64_t requested{};
        std::uint32_t admitted{};
        double carry{};
    };

    struct CpuParticleSpawnStageResult final {
        CpuParticleBirthPlan plan{};
        std::uint64_t nextSimulationIdentity{};
    };

    struct CpuParticleCollisionSelection final {
        bool hit{};
        Math::Vec3 position{};
        Math::Vec3 normal{0.0F, 1.0F, 0.0F};
        float distance{};
        float restitution{1.0F};
        std::uint64_t stableTarget{};
        std::uint32_t stableFeature{};
    };

    struct CpuParticleCompiledPayloadModule final {
        CpuParticleStage stage{CpuParticleStage::Integrate};
        CpuParticlePayloadOperation operation{CpuParticlePayloadOperation::Affine};
        std::uint16_t readChannel{};
        std::uint16_t writeChannel{};
        std::uint32_t readStream{};
        std::uint32_t writeStream{};
        float scale{1.0F};
        float bias{};
        float threshold{0.5F};
        float belowValue{};
        float atOrAboveValue{1.0F};
    };

    struct CpuParticleSimulatorState final {
        CpuParticleSimulatorState(CpuParticleBuffer committedBuffer, CpuParticleBuffer candidateBuffer)
            : committed(std::move(committedBuffer)), candidate(std::move(candidateBuffer)) {}

        CpuParticleSimulatorState(const CpuParticleSimulatorState &) = delete;
        CpuParticleSimulatorState &operator=(const CpuParticleSimulatorState &) = delete;

        CpuParticleBuffer committed;
        CpuParticleBuffer candidate;
        ParticleSystemDescriptorData descriptor{};
        ParticleBufferId buffer{};
        EffectSystemId activation{};
        std::uint64_t effectSeed{};
        std::thread::id ownerThread{};
        std::uint32_t capacity{};
        std::uint32_t customFloatStreams{};
        bool requiredGameplay{};
        std::array<CpuParticleForceModule, CpuParticleSimulationHardLimits::ForceModules> forces{};
        std::uint32_t forceCount{};
        std::array<CpuParticlePlane, CpuParticleSimulationHardLimits::Planes> planes{};
        std::uint32_t planeCount{};
        CpuParticleCollisionQuerySeam sceneDepth{};
        CpuParticleCollisionQuerySeam physicsWorld{};
        CpuParticleCollisionResponse collisionResponse{CpuParticleCollisionResponse::Bounce};
        std::array<CpuParticleCurveKey, CpuParticleSimulationHardLimits::CurveKeys> sizeOverLife{};
        std::uint32_t sizeOverLifeCount{};
        std::array<CpuParticleCurveKey, CpuParticleSimulationHardLimits::CurveKeys> opacityOverLife{};
        std::uint32_t opacityOverLifeCount{};
        std::array<CpuParticleColorKey, CpuParticleSimulationHardLimits::CurveKeys> colorOverLife{};
        std::uint32_t colorOverLifeCount{};
        std::array<CpuParticlePayloadChannel, CpuParticleSimulationHardLimits::PayloadChannels> payloadChannels{};
        std::array<float, CpuParticleSimulationHardLimits::PayloadChannels> inputValues{};
        std::uint32_t payloadChannelCount{};
        std::array<CpuParticleCompiledPayloadModule, CpuParticleSimulationHardLimits::PayloadChannels> payloadModules{};
        std::array<bool, CpuParticleSimulationHardLimits::PayloadChannels> outputHasModule{};
        std::uint32_t payloadModuleCount{};
        CpuParticleStageObserver stageObserver{};
        void *stageObserverContext{};

        std::vector<CpuParticleHandle> handles;
        std::vector<CpuParticleHandle> killRequests;
        std::vector<std::uint32_t> survivorSources;
        std::vector<Math::Vec3> previousPositions;
        std::vector<Math::Vec3> acceleration;
        std::vector<float> baseSizeX;
        std::vector<float> baseSizeY;
        std::vector<Math::Vec4> baseColor;

        std::uint32_t committedOffset{};
        std::uint32_t candidateOffset{};
        float maximumDeltaSeconds{};
        std::uint32_t maximumBurstParticles{};
        double spawnCarry{};
        float spawnRate{};
        std::uint64_t nextSimulationIdentity{};
        std::uint64_t nextTick{};
        std::uint64_t committedGeneration{};
        std::uint64_t steps{};
        std::uint64_t spawned{};
        std::uint64_t dropped{};
        std::uint64_t killed{};
        std::uint64_t collisions{};
        bool shutDown{};
        bool advancing{};
    };
}  // namespace Horo::Vfx::Detail

namespace Horo::Vfx::CpuParticleSimulatorDetail {
    template <typename T> [[nodiscard]] inline Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    [[nodiscard]] inline bool Finite(const float value) noexcept {
        return std::isfinite(value);
    }

    [[nodiscard]] inline bool Finite(const Math::Vec3 value) noexcept {
        return Finite(value.x) && Finite(value.y) && Finite(value.z);
    }

    [[nodiscard]] inline bool Finite(const Math::Vec4 value) noexcept {
        return Finite(value.x) && Finite(value.y) && Finite(value.z) && Finite(value.w);
    }

    [[nodiscard]] inline float ClampUnit(const float value) noexcept {
        return std::clamp(value, 0.0F, 1.0F);
    }

    [[nodiscard]] inline std::uint32_t PackChannel(const float value) noexcept {
        return static_cast<std::uint32_t>(ClampUnit(value) * 255.0F + 0.5F);
    }

    [[nodiscard]] inline std::uint32_t PackColor(const Math::Vec4 color) noexcept {
        return (PackChannel(color.x) << 24U) | (PackChannel(color.y) << 16U) | (PackChannel(color.z) << 8U) | PackChannel(color.w);
    }

    [[nodiscard]] inline std::uint64_t Mix(std::uint64_t value) noexcept {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    inline void HashCombine(std::uint64_t &hash, const std::uint64_t value) noexcept {
        hash ^= Mix(value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U));
    }

    [[nodiscard]] inline std::uint64_t IdentityBase(const CpuParticleSimulatorCreateInfo &info,
                                                    const ParticleSystemDescriptorData &descriptor) noexcept {
        std::uint64_t hash = Mix(info.effectSeed);
        HashCombine(hash, info.activation.scope.Value());
        HashCombine(hash, info.activation.slot);
        HashCombine(hash, info.activation.generation);
        HashCombine(hash, descriptor.emitter.scope.Value());
        HashCombine(hash, descriptor.emitter.slot);
        HashCombine(hash, descriptor.emitter.generation);
        return hash & 0x0000FFFFFFFFFFFFULL;
    }

    [[nodiscard]] inline std::uint64_t RandomWord(const Detail::CpuParticleSimulatorState &state, const ParticleSimulationId particle,
                                                  const std::uint32_t channel, const std::uint32_t sampleOrdinal,
                                                  const std::uint64_t tick) noexcept {
        std::uint64_t hash = Mix(state.effectSeed);
        HashCombine(hash, state.activation.scope.Value());
        HashCombine(hash, state.activation.slot);
        HashCombine(hash, state.activation.generation);
        HashCombine(hash, state.descriptor.emitter.scope.Value());
        HashCombine(hash, state.descriptor.emitter.slot);
        HashCombine(hash, state.descriptor.emitter.generation);
        HashCombine(hash, particle.Value());
        HashCombine(hash, channel);
        HashCombine(hash, sampleOrdinal);
        HashCombine(hash, tick);
        HashCombine(hash, CpuParticleSimulationHardLimits::RandomAlgorithmVersion);
        return hash;
    }

    [[nodiscard]] inline float UnitFloat(const Detail::CpuParticleSimulatorState &state, const ParticleSimulationId particle,
                                         const std::uint32_t channel, const std::uint32_t sampleOrdinal,
                                         const std::uint64_t tick) noexcept {
        constexpr float Scale = 1.0F / 16'777'216.0F;
        return static_cast<float>(RandomWord(state, particle, channel, sampleOrdinal, tick) >> 40U) * Scale;
    }

    [[nodiscard]] inline float SampleRange(const Detail::CpuParticleSimulatorState &state, const ParticleSimulationId particle,
                                           const std::uint32_t channel, const ParticleScalarRange range,
                                           const std::uint64_t tick = 0) noexcept {
        const double unit = UnitFloat(state, particle, channel, 0, tick);
        return static_cast<float>(range.minimum + ((range.maximum - range.minimum) * unit));
    }

    [[nodiscard]] Result<void> OwnerThreadResult(const Detail::CpuParticleSimulatorState &state);
    [[nodiscard]] Result<void> ValidateOperationalState(const Detail::CpuParticleSimulatorState *state);
    [[nodiscard]] Result<void> Observe(const Detail::CpuParticleSimulatorState &state, CpuParticleStage stage);

    [[nodiscard]] Result<std::unique_ptr<Detail::CpuParticleSimulatorState>> PrepareState(const ParticleSystemDescriptor &descriptor,
                                                                                          const CpuParticleSimulatorCreateInfo &info);

    [[nodiscard]] Result<void> PrepareCandidate(Detail::CpuParticleSimulatorState &state);
    [[nodiscard]] Result<Detail::CpuParticleSpawnStageResult> RunSpawnStage(Detail::CpuParticleSimulatorState &state,
                                                                            const CpuParticleSimulationStep &step);
    [[nodiscard]] Result<void> RunInitializeStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step,
                                                  const Detail::CpuParticleSpawnStageResult &spawn);
    [[nodiscard]] Result<void> RunForcesStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step);
    [[nodiscard]] Result<void> RunIntegrateStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step);
    [[nodiscard]] Result<std::uint32_t> RunCollideStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step);
    [[nodiscard]] Result<std::uint32_t> RunKillStage(Detail::CpuParticleSimulatorState &state);
    void CommitAuxiliary(Detail::CpuParticleSimulatorState &state, std::uint32_t survivorCount) noexcept;
}  // namespace Horo::Vfx::CpuParticleSimulatorDetail
