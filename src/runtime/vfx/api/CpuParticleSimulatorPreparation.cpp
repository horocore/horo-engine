#include "CpuParticleSimulatorInternal.h"
#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Vfx::CpuParticleSimulatorDetail {
    namespace {
        struct PreparedBuffers final {
            CpuParticleBuffer committed;
            CpuParticleBuffer candidate;
            std::uint32_t customFloatStreams{};
        };

        [[nodiscard]] bool ValidCurve(const std::span<const CpuParticleCurveKey> keys) noexcept {
            if (keys.size() > CpuParticleSimulationHardLimits::CurveKeys)
                return false;
            float previous = -1.0F;
            for (const auto &key : keys) {
                if (!Finite(key.normalizedAge) || !Finite(key.value) || key.normalizedAge < 0.0F || key.normalizedAge > 1.0F ||
                    key.normalizedAge < previous)
                    return false;
                previous = key.normalizedAge;
            }
            return true;
        }

        [[nodiscard]] bool ValidColorCurve(const std::span<const CpuParticleColorKey> keys) noexcept {
            if (keys.size() > CpuParticleSimulationHardLimits::CurveKeys)
                return false;
            float previous = -1.0F;
            for (const auto &key : keys) {
                if (!Finite(key.normalizedAge) || !Finite(key.color) || key.normalizedAge < 0.0F || key.normalizedAge > 1.0F ||
                    key.normalizedAge < previous)
                    return false;
                previous = key.normalizedAge;
            }
            return true;
        }

        [[nodiscard]] bool ValidForce(const CpuParticleForceModule &force) noexcept {
            using enum CpuParticleForceKind;
            if (force.kind >= Count || !Finite(force.vector) || !Finite(force.center) || !Finite(force.strength) ||
                !Finite(force.falloff) || !Finite(force.frequency) || force.falloff < 0.0F || force.frequency < 0.0F)
                return false;
            if (force.kind == Noise)
                return force.randomChannel >= 10 && Finite(force.strength * force.frequency);
            if (force.kind == Gravity || force.kind == Wind)
                return Finite(force.vector * force.strength);
            return true;
        }

        [[nodiscard]] Result<void> ValidateBasic(const ParticleSystemDescriptorData &data, const CpuParticleSimulatorCreateInfo &info) {
            if (!info.buffer.IsValid() || !info.activation.IsValid() || !data.emitter.IsValid() || info.maximumBurstParticles == 0 ||
                info.maximumBurstParticles > CpuParticleSimulationHardLimits::BurstParticles || !Finite(info.maximumDeltaSeconds) ||
                info.maximumDeltaSeconds <= 0.0F || info.maximumDeltaSeconds > CpuParticleSimulationHardLimits::DeltaSeconds ||
                data.maximumParticles == 0 || data.maximumParticles > CpuParticleBufferHardLimits::Particles ||
                data.collisionMode >= ParticleCollisionMode::Count || info.collisionResponse >= CpuParticleCollisionResponse::Count ||
                info.forces.size() > CpuParticleSimulationHardLimits::ForceModules ||
                info.planes.size() > CpuParticleSimulationHardLimits::Planes ||
                info.payloadChannels.size() > CpuParticleSimulationHardLimits::PayloadChannels || !ValidCurve(info.sizeOverLife) ||
                !ValidCurve(info.opacityOverLife) || !ValidColorCurve(info.colorOverLife))
                return Failure<void>(VfxErrors::ParticleSimulationDescriptorInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateForces(const std::span<const CpuParticleForceModule> forces) {
            std::array<bool, 256> noiseChannelSeen{};
            for (const auto &force : forces) {
                if (!ValidForce(force))
                    return Failure<void>(VfxErrors::ParticleSimulationDescriptorInvalid);
                if (force.kind == CpuParticleForceKind::Noise) {
                    if (noiseChannelSeen[force.randomChannel])
                        return Failure<void>(VfxErrors::ParticleSimulationDescriptorInvalid);
                    noiseChannelSeen[force.randomChannel] = true;
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint32_t> DetermineCustomFloatStreams(const std::span<const CpuParticlePayloadChannel> channels,
                                                                        const std::uint32_t configuredStreams) {
            std::uint32_t streams = configuredStreams;
            for (const auto &channel : channels) {
                if (channel.channel == 0 || channel.classification >= CpuParticlePayloadClass::Count || !Finite(channel.minimum) ||
                    !Finite(channel.maximum) || channel.minimum > channel.maximum ||
                    channel.customFloatStream == std::numeric_limits<std::uint32_t>::max())
                    return Failure<std::uint32_t>(VfxErrors::ParticlePayloadSchemaMismatch);
                for (const auto &other : channels) {
                    if (&channel != &other && channel.channel == other.channel)
                        return Failure<std::uint32_t>(VfxErrors::ParticlePayloadSchemaMismatch);
                }
                const std::uint32_t requiredStreams = channel.customFloatStream + 1U;
                if (configuredStreams != 0 && requiredStreams > configuredStreams)
                    return Failure<std::uint32_t>(VfxErrors::ParticlePayloadSchemaMismatch);
                streams = std::max(streams, requiredStreams);
            }
            if (streams > CpuParticleBufferHardLimits::CustomFloatStreams)
                return Failure<std::uint32_t>(VfxErrors::ParticlePayloadSchemaMismatch);
            return Result<std::uint32_t>::Success(streams);
        }

        [[nodiscard]] Result<void> ValidateCollision(const ParticleSystemDescriptorData &data, const CpuParticleSimulatorCreateInfo &info) {
            for (const auto &plane : info.planes) {
                if (!Finite(plane.point) || !Finite(plane.normal) || !Finite(plane.restitution) || plane.restitution < 0.0F ||
                    plane.restitution > 1.0F || Math::LengthSquared(plane.normal) <= std::numeric_limits<float>::epsilon())
                    return Failure<void>(VfxErrors::ParticleSimulationDescriptorInvalid);
            }
            if (data.collisionMode == ParticleCollisionMode::Planes && info.planes.empty())
                return Failure<void>(VfxErrors::ParticleSimulationDescriptorInvalid);
            const auto seamUnavailable = [](const CpuParticleCollisionQuerySeam &seam) {
                return seam.required && seam.probe == nullptr;
            };
            if ((data.collisionMode == ParticleCollisionMode::SceneDepth && seamUnavailable(info.sceneDepth)) ||
                (data.collisionMode == ParticleCollisionMode::PhysicsWorld && seamUnavailable(info.physicsWorld)))
                return Failure<void>(VfxErrors::ParticleCollisionQueryUnavailable);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<PreparedBuffers> CreateBuffers(const ParticleSystemDescriptorData &data,
                                                            const CpuParticleSimulatorCreateInfo &info,
                                                            const std::uint32_t customFloatStreams) {
            auto committed = CpuParticleBuffer::Create({.buffer = info.buffer,
                                                        .capacity = data.maximumParticles,
                                                        .customFloatStreams = customFloatStreams,
                                                        .maximumBytes = info.maximumBufferBytes});
            if (committed.HasError())
                return Result<PreparedBuffers>::Failure(committed.ErrorValue());
            auto candidate = CpuParticleBuffer::Create({.buffer = info.buffer,
                                                        .capacity = data.maximumParticles,
                                                        .customFloatStreams = customFloatStreams,
                                                        .maximumBytes = info.maximumBufferBytes});
            if (candidate.HasError())
                return Result<PreparedBuffers>::Failure(candidate.ErrorValue());
            return Result<PreparedBuffers>::Success({.committed = std::move(committed).Value(),
                                                     .candidate = std::move(candidate).Value(),
                                                     .customFloatStreams = customFloatStreams});
        }

        void CopyPreparedInputs(Detail::CpuParticleSimulatorState &state, const ParticleSystemDescriptorData &data,
                                const CpuParticleSimulatorCreateInfo &info) {
            state.descriptor = data;
            state.buffer = info.buffer;
            state.activation = info.activation;
            state.effectSeed = info.effectSeed;
            state.ownerThread = std::this_thread::get_id();
            state.capacity = data.maximumParticles;
            state.requiredGameplay = info.requiredGameplay;
            state.maximumDeltaSeconds = info.maximumDeltaSeconds;
            state.maximumBurstParticles = info.maximumBurstParticles;
            state.sceneDepth = info.sceneDepth;
            state.physicsWorld = info.physicsWorld;
            state.collisionResponse = info.collisionResponse;
            state.stageObserver = info.stageObserver;
            state.stageObserverContext = info.stageObserverContext;
        }

        void CopyCompiledKernels(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulatorCreateInfo &info) {
            state.forceCount = static_cast<std::uint32_t>(info.forces.size());
            std::copy(info.forces.begin(), info.forces.end(), state.forces.begin());
            state.planeCount = static_cast<std::uint32_t>(info.planes.size());
            for (std::uint32_t index = 0; index < state.planeCount; ++index) {
                state.planes[index] = info.planes[index];
                state.planes[index].normal = Math::Normalize(state.planes[index].normal);
            }
            state.sizeOverLifeCount = static_cast<std::uint32_t>(info.sizeOverLife.size());
            std::copy(info.sizeOverLife.begin(), info.sizeOverLife.end(), state.sizeOverLife.begin());
            state.opacityOverLifeCount = static_cast<std::uint32_t>(info.opacityOverLife.size());
            std::copy(info.opacityOverLife.begin(), info.opacityOverLife.end(), state.opacityOverLife.begin());
            state.colorOverLifeCount = static_cast<std::uint32_t>(info.colorOverLife.size());
            std::copy(info.colorOverLife.begin(), info.colorOverLife.end(), state.colorOverLife.begin());
        }

        void CopyPayloadSchema(Detail::CpuParticleSimulatorState &state, const std::span<const CpuParticlePayloadChannel> channels) {
            state.payloadChannelCount = static_cast<std::uint32_t>(channels.size());
            std::copy(channels.begin(), channels.end(), state.payloadChannels.begin());
            for (std::uint32_t index = 0; index < state.payloadChannelCount; ++index)
                state.inputValues[index] = state.payloadChannels[index].minimum;
        }

        void AllocateScratch(Detail::CpuParticleSimulatorState &state) {
            const std::size_t doubledCapacity = static_cast<std::size_t>(state.capacity) * 2U;
            state.handles.resize(doubledCapacity);
            state.baseSizeX.resize(doubledCapacity);
            state.baseSizeY.resize(doubledCapacity);
            state.baseColor.resize(doubledCapacity);
            state.survivorSources.resize(state.capacity);
            state.previousPositions.resize(state.capacity);
            state.acceleration.resize(state.capacity);
            state.killRequests.reserve(state.capacity);
            state.committedOffset = 0;
            state.candidateOffset = state.capacity;
        }

        void InitializeCursors(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulatorCreateInfo &info,
                               const ParticleSystemDescriptorData &data) {
            const std::uint64_t identityBase = IdentityBase(info, data);
            state.nextSimulationIdentity = identityBase == 0 ? 1 : identityBase;
            state.spawnRate = SampleRange(state, ParticleSimulationId::Create(state.nextSimulationIdentity).Value(), 4, data.spawnRate);
        }
    }  // namespace

    /** @brief Validates, allocates, and initializes one complete CPU simulator state. */
    Result<std::unique_ptr<Detail::CpuParticleSimulatorState>> PrepareState(const ParticleSystemDescriptor &descriptor,
                                                                            const CpuParticleSimulatorCreateInfo &info) {
        const auto &data = descriptor.Data();
        if (auto result = ValidateBasic(data, info); result.HasError())
            return Result<std::unique_ptr<Detail::CpuParticleSimulatorState>>::Failure(result.ErrorValue());
        if (auto result = ValidateForces(info.forces); result.HasError())
            return Result<std::unique_ptr<Detail::CpuParticleSimulatorState>>::Failure(result.ErrorValue());
        if (auto result = ValidateCollision(data, info); result.HasError())
            return Result<std::unique_ptr<Detail::CpuParticleSimulatorState>>::Failure(result.ErrorValue());
        const auto customStreams = DetermineCustomFloatStreams(info.payloadChannels, info.customFloatStreams);
        if (customStreams.HasError())
            return Result<std::unique_ptr<Detail::CpuParticleSimulatorState>>::Failure(customStreams.ErrorValue());
        auto buffers = CreateBuffers(data, info, customStreams.Value());
        if (buffers.HasError())
            return Result<std::unique_ptr<Detail::CpuParticleSimulatorState>>::Failure(buffers.ErrorValue());
        try {
            auto prepared = std::move(buffers).Value();
            auto state = std::make_unique<Detail::CpuParticleSimulatorState>(std::move(prepared.committed), std::move(prepared.candidate));
            state->customFloatStreams = customStreams.Value();
            CopyPreparedInputs(*state, data, info);
            CopyCompiledKernels(*state, info);
            CopyPayloadSchema(*state, info.payloadChannels);
            AllocateScratch(*state);
            InitializeCursors(*state, info, data);
            return Result<std::unique_ptr<Detail::CpuParticleSimulatorState>>::Success(std::move(state));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<Detail::CpuParticleSimulatorState>>(VfxErrors::ParticleBufferAllocationFailed);
        }
    }
}  // namespace Horo::Vfx::CpuParticleSimulatorDetail
