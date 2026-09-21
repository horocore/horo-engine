#include "Horo/Vfx/CpuParticleSimulator.h"

#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Vfx {
    namespace {
        constexpr std::uint32_t ExplicitKillBit = 1U << 0U;
        constexpr float Tau = 6.28318530717958647692F;
        constexpr float CollisionEpsilon = 0.0001F;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool Finite(const float value) noexcept {
            return std::isfinite(value);
        }

        [[nodiscard]] bool Finite(const Math::Vec3 value) noexcept {
            return Finite(value.x) && Finite(value.y) && Finite(value.z);
        }

        [[nodiscard]] bool Finite(const Math::Vec4 value) noexcept {
            return Finite(value.x) && Finite(value.y) && Finite(value.z) && Finite(value.w);
        }

        [[nodiscard]] float ClampUnit(const float value) noexcept {
            return std::clamp(value, 0.0F, 1.0F);
        }

        [[nodiscard]] std::uint32_t PackChannel(const float value) noexcept {
            return static_cast<std::uint32_t>(ClampUnit(value) * 255.0F + 0.5F);
        }

        [[nodiscard]] std::uint32_t PackColor(const Math::Vec4 color) noexcept {
            return (PackChannel(color.x) << 24U) | (PackChannel(color.y) << 16U) | (PackChannel(color.z) << 8U) |
                   PackChannel(color.w);
        }

        [[nodiscard]] std::uint64_t Mix(std::uint64_t value) noexcept {
            value += 0x9E3779B97F4A7C15ULL;
            value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        }

        void HashCombine(std::uint64_t &hash, const std::uint64_t value) noexcept {
            hash ^= Mix(value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U));
        }

        [[nodiscard]] std::uint64_t IdentityBase(const CpuParticleSimulatorCreateInfo &info,
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

        struct BirthPlan final {
            std::uint64_t requested{};
            std::uint32_t admitted{};
            double carry{};
        };

        struct CollisionSelection final {
            bool hit{};
            Math::Vec3 position{};
            Math::Vec3 normal{0.0F, 1.0F, 0.0F};
            float distance{};
            float restitution{1.0F};
            std::uint64_t stableTarget{};
            std::uint32_t stableFeature{};
        };
    }  // namespace

    namespace Detail {
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
        };
    }  // namespace Detail

    namespace {
        [[nodiscard]] Result<void> OwnerThreadResult(const Detail::CpuParticleSimulatorState &state) {
            if (std::this_thread::get_id() != state.ownerThread)
                return Failure<void>(VfxErrors::ParticleBufferThreadViolation);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateOperationalState(const Detail::CpuParticleSimulatorState *const state) {
            if (state == nullptr || state->shutDown)
                return Failure<void>(VfxErrors::ParticleBufferShutDown);
            return OwnerThreadResult(*state);
        }

        [[nodiscard]] std::uint64_t RandomWord(const Detail::CpuParticleSimulatorState &state,
                                                const ParticleSimulationId particle, const std::uint32_t channel,
                                                const std::uint32_t sampleOrdinal, const std::uint64_t tick) noexcept {
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

        [[nodiscard]] float UnitFloat(const Detail::CpuParticleSimulatorState &state, const ParticleSimulationId particle,
                                      const std::uint32_t channel, const std::uint32_t sampleOrdinal,
                                      const std::uint64_t tick) noexcept {
            constexpr float Scale = 1.0F / 16'777'216.0F;
            return static_cast<float>(RandomWord(state, particle, channel, sampleOrdinal, tick) >> 40U) * Scale;
        }

        [[nodiscard]] float SampleRange(const Detail::CpuParticleSimulatorState &state, const ParticleSimulationId particle,
                                        const std::uint32_t channel, const ParticleScalarRange range,
                                        const std::uint64_t tick = 0) noexcept {
            const double unit = UnitFloat(state, particle, channel, 0, tick);
            return static_cast<float>(range.minimum + ((range.maximum - range.minimum) * unit));
        }

        [[nodiscard]] Result<void> Observe(const Detail::CpuParticleSimulatorState &state, const CpuParticleStage stage) {
            if (state.stageObserver == nullptr)
                return Result<void>::Success();
            return state.stageObserver(state.stageObserverContext, stage);
        }

        [[nodiscard]] Result<BirthPlan> CalculateBirthPlan(const Detail::CpuParticleSimulatorState &state,
                                                           const CpuParticleSimulationStep &step) {
            const double continuous = state.spawnCarry + (static_cast<double>(state.spawnRate) * step.deltaSeconds);
            if (!std::isfinite(continuous) || continuous < 0.0)
                return Failure<BirthPlan>(VfxErrors::ParticleSimulationDescriptorInvalid);
            const double integral = std::floor(continuous);
            const auto continuousBirths = integral >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                                              ? std::numeric_limits<std::uint64_t>::max()
                                              : static_cast<std::uint64_t>(integral);
            if (continuousBirths > std::numeric_limits<std::uint64_t>::max() - step.burstCount)
                return Failure<BirthPlan>(VfxErrors::ParticleSimulationDescriptorInvalid);
            const std::uint64_t requested = continuousBirths + step.burstCount;
            const std::uint32_t available = state.candidate.Statistics().available;
            if (state.requiredGameplay && requested > available)
                return Failure<BirthPlan>(VfxErrors::ParticleStepCapacityExceeded);
            const auto admitted = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, available));
            if (state.nextSimulationIdentity > std::numeric_limits<std::uint64_t>::max() - admitted)
                return Failure<BirthPlan>(VfxErrors::ParticleSpawnOrdinalExhausted);
            return Result<BirthPlan>::Success(
                {.requested = requested, .admitted = admitted, .carry = continuous - integral});
        }

        void InitializePosition(const Detail::CpuParticleSimulatorState &state, const ParticleSimulationId particle,
                                const CpuParticleSoAView &view, const std::uint32_t dense, const std::uint64_t tick) noexcept {
            using enum ParticleEmitterShape;
            const double u = UnitFloat(state, particle, 1, 0, tick);
            const double v = UnitFloat(state, particle, 2, 0, tick);
            const double w = UnitFloat(state, particle, 3, 0, tick);
            double x{};
            double y{};
            double z{};
            switch (state.descriptor.shape) {
                case Point:
                    break;
                case Sphere: {
                    const double cosine = (2.0 * v) - 1.0;
                    const double radius = std::cbrt(w);
                    const double radial = std::sqrt(std::max(0.0, 1.0 - (cosine * cosine))) * radius;
                    const double angle = static_cast<double>(Tau) * u;
                    x = radial * std::cos(angle);
                    y = radial * std::sin(angle);
                    z = cosine * radius;
                    break;
                }
                case Box:
                    x = (2.0 * u) - 1.0;
                    y = (2.0 * v) - 1.0;
                    z = (2.0 * w) - 1.0;
                    break;
                case Cone: {
                    z = std::cbrt(w);
                    const double radial = std::sqrt(v) * z;
                    const double angle = static_cast<double>(Tau) * u;
                    x = radial * std::cos(angle);
                    y = radial * std::sin(angle);
                    break;
                }
                case Count:
                    break;
            }
            view.positionX[dense] = static_cast<float>(x);
            view.positionY[dense] = static_cast<float>(y);
            view.positionZ[dense] = static_cast<float>(z);
        }

        void InitializeParticle(Detail::CpuParticleSimulatorState &state, const CpuParticleHandle &handle,
                                const CpuParticleSoAView &view, const std::uint32_t dense,
                                const std::uint64_t tick) noexcept {
            InitializePosition(state, handle.particle, view, dense, tick);
            double x = view.positionX[dense];
            double y = view.positionY[dense];
            double z = view.positionZ[dense];
            double length = std::sqrt((x * x) + (y * y) + (z * z));
            if (length <= std::numeric_limits<double>::epsilon()) {
                const double cosine = (2.0 * UnitFloat(state, handle.particle, 2, 1, tick)) - 1.0;
                const double angle = static_cast<double>(Tau) * UnitFloat(state, handle.particle, 1, 1, tick);
                const double radial = std::sqrt(std::max(0.0, 1.0 - (cosine * cosine)));
                x = radial * std::cos(angle);
                y = radial * std::sin(angle);
                z = cosine;
                length = 1.0;
            }
            const float speed = SampleRange(state, handle.particle, 5, state.descriptor.initialSpeed, tick);
            const float size = SampleRange(state, handle.particle, 6, state.descriptor.initialSize, tick);
            const float opacity = SampleRange(state, handle.particle, 7, state.descriptor.initialOpacity, tick);
            const float lifetime = state.descriptor.lifetimeKind == ParticleLifetimeKind::Finite
                                       ? SampleRange(state, handle.particle, 8, state.descriptor.lifetimeSeconds, tick)
                                       : std::numeric_limits<float>::max();
            view.velocityX[dense] = static_cast<float>((x / length) * speed);
            view.velocityY[dense] = static_cast<float>((y / length) * speed);
            view.velocityZ[dense] = static_cast<float>((z / length) * speed);
            view.sizeX[dense] = size;
            view.sizeY[dense] = size;
            view.rotation[dense] = Tau * UnitFloat(state, handle.particle, 9, 0, tick);
            view.angularVelocity[dense] = 0.0F;
            view.age[dense] = 0.0F;
            view.maximumAge[dense] = lifetime;
            view.packedColor[dense] = PackColor({1.0F, 1.0F, 1.0F, opacity});
            view.customFlags[dense] = 0U;
            for (std::uint32_t stream = 0; stream < view.customFloatStreamCount; ++stream)
                view.customFloats[stream][dense] = 0.0F;

            const std::uint32_t auxiliary = state.candidateOffset + dense;
            state.baseSizeX[auxiliary] = size;
            state.baseSizeY[auxiliary] = size;
            state.baseColor[auxiliary] = {1.0F, 1.0F, 1.0F, opacity};
            for (std::uint32_t channelIndex = 0; channelIndex < state.payloadChannelCount; ++channelIndex) {
                const auto &channel = state.payloadChannels[channelIndex];
                if (channel.classification == CpuParticlePayloadClass::GameplayInput)
                    view.customFloats[channel.customFloatStream][dense] = state.inputValues[channelIndex];
            }
        }

        [[nodiscard]] float EvaluateCurve(const std::array<CpuParticleCurveKey, CpuParticleSimulationHardLimits::CurveKeys> &keys,
                                           const std::uint32_t count, const float normalizedAge,
                                           const float fallback) noexcept {
            if (count == 0)
                return fallback;
            if (normalizedAge <= keys[0].normalizedAge)
                return keys[0].value;
            for (std::uint32_t index = 1; index < count; ++index) {
                if (normalizedAge > keys[index].normalizedAge)
                    continue;
                const float span = keys[index].normalizedAge - keys[index - 1U].normalizedAge;
                if (span <= 0.0F)
                    return keys[index].value;
                const float alpha = (normalizedAge - keys[index - 1U].normalizedAge) / span;
                return keys[index - 1U].value + ((keys[index].value - keys[index - 1U].value) * alpha);
            }
            return keys[count - 1U].value;
        }

        [[nodiscard]] Math::Vec4 EvaluateColorCurve(
            const std::array<CpuParticleColorKey, CpuParticleSimulationHardLimits::CurveKeys> &keys,
            const std::uint32_t count, const float normalizedAge, const Math::Vec4 fallback) noexcept {
            if (count == 0)
                return fallback;
            if (normalizedAge <= keys[0].normalizedAge)
                return keys[0].color;
            for (std::uint32_t index = 1; index < count; ++index) {
                if (normalizedAge > keys[index].normalizedAge)
                    continue;
                const float span = keys[index].normalizedAge - keys[index - 1U].normalizedAge;
                if (span <= 0.0F)
                    return keys[index].color;
                const float alpha = (normalizedAge - keys[index - 1U].normalizedAge) / span;
                return Math::Lerp(keys[index - 1U].color, keys[index].color, alpha);
            }
            return keys[count - 1U].color;
        }

        void ApplyOverLife(Detail::CpuParticleSimulatorState &state, const CpuParticleSoAView &view,
                           const std::uint32_t dense) noexcept {
            const float maximumAge = view.maximumAge[dense];
            const float normalizedAge = maximumAge > 0.0F && Finite(maximumAge) ? ClampUnit(view.age[dense] / maximumAge) : 0.0F;
            const std::uint32_t auxiliary = state.candidateOffset + dense;
            const float sizeMultiplier = EvaluateCurve(state.sizeOverLife, state.sizeOverLifeCount, normalizedAge, 1.0F);
            view.sizeX[dense] = state.baseSizeX[auxiliary] * sizeMultiplier;
            view.sizeY[dense] = state.baseSizeY[auxiliary] * sizeMultiplier;
            const float opacityMultiplier = EvaluateCurve(state.opacityOverLife, state.opacityOverLifeCount, normalizedAge, 1.0F);
            Math::Vec4 color = EvaluateColorCurve(state.colorOverLife, state.colorOverLifeCount, normalizedAge,
                                                   state.baseColor[auxiliary]);
            color.w *= opacityMultiplier;
            view.packedColor[dense] = PackColor(color);
            for (std::uint32_t channelIndex = 0; channelIndex < state.payloadChannelCount; ++channelIndex) {
                const auto &channel = state.payloadChannels[channelIndex];
                if (channel.classification == CpuParticlePayloadClass::GameplayOutput) {
                    const float output = channel.minimum + ((channel.maximum - channel.minimum) * normalizedAge);
                    view.customFloats[channel.customFloatStream][dense] = output;
                } else if (channel.classification == CpuParticlePayloadClass::GameplayInput) {
                    view.customFloats[channel.customFloatStream][dense] = state.inputValues[channelIndex];
                }
            }
        }

        void ApplyForces(const Detail::CpuParticleSimulatorState &state, const CpuParticleSoAView &view,
                         const std::uint32_t dense, const std::uint64_t tick, const ParticleSimulationId particle,
                         Math::Vec3 &acceleration) noexcept {
            acceleration = {};
            const Math::Vec3 position{view.positionX[dense], view.positionY[dense], view.positionZ[dense]};
            for (std::uint32_t forceIndex = 0; forceIndex < state.forceCount; ++forceIndex) {
                const auto &force = state.forces[forceIndex];
                switch (force.kind) {
                    case CpuParticleForceKind::Gravity:
                    case CpuParticleForceKind::Wind:
                        acceleration += force.vector * force.strength;
                        break;
                    case CpuParticleForceKind::Attraction: {
                        const Math::Vec3 delta = force.center - position;
                        const float lengthSquared = Math::LengthSquared(delta);
                        if (lengthSquared <= std::numeric_limits<float>::epsilon())
                            break;
                        const float length = std::sqrt(lengthSquared);
                        const float attenuation = 1.0F / (1.0F + (std::max(0.0F, force.falloff) * length));
                        acceleration += (delta / length) * (force.strength * attenuation);
                        break;
                    }
                    case CpuParticleForceKind::Noise: {
                        const std::uint32_t channel = force.randomChannel == 0 ? 1U : force.randomChannel;
                        const float x = (UnitFloat(state, particle, channel, 0, tick) * 2.0F) - 1.0F;
                        const float y = (UnitFloat(state, particle, channel, 1, tick) * 2.0F) - 1.0F;
                        const float z = (UnitFloat(state, particle, channel, 2, tick) * 2.0F) - 1.0F;
                        acceleration += Math::Vec3{x, y, z} * (force.strength * force.frequency);
                        break;
                    }
                    case CpuParticleForceKind::Count:
                        break;
                }
            }
        }

        [[nodiscard]] Result<CollisionSelection> QueryCollision(const Detail::CpuParticleSimulatorState &state,
                                                                const CpuParticleSimulationStep &step,
                                                                const CpuParticleHandle &handle,
                                                                const Math::Vec3 previous, const Math::Vec3 position,
                                                                const Math::Vec3 velocity) {
            CollisionSelection selection{};
            switch (state.descriptor.collisionMode) {
                case ParticleCollisionMode::None:
                    return Result<CollisionSelection>::Success(selection);
                case ParticleCollisionMode::Planes: {
                    float bestTime = std::numeric_limits<float>::max();
                    for (std::uint32_t planeIndex = 0; planeIndex < state.planeCount; ++planeIndex) {
                        const auto &plane = state.planes[planeIndex];
                        const float previousDistance = Math::Dot(previous - plane.point, plane.normal);
                        const float currentDistance = Math::Dot(position - plane.point, plane.normal);
                        if (previousDistance < 0.0F || currentDistance > 0.0F)
                            continue;
                        const float denominator = previousDistance - currentDistance;
                        const float time = denominator > std::numeric_limits<float>::epsilon() ? previousDistance / denominator : 0.0F;
                        if (time >= bestTime)
                            continue;
                        bestTime = time;
                        selection.hit = true;
                        selection.normal = plane.normal;
                        selection.position = previous + ((position - previous) * ClampUnit(time));
                        selection.distance = Math::Length(selection.position - previous);
                        selection.restitution = plane.restitution;
                        selection.stableTarget = planeIndex;
                        selection.stableFeature = planeIndex;
                    }
                    return Result<CollisionSelection>::Success(selection);
                }
                case ParticleCollisionMode::SceneDepth:
                case ParticleCollisionMode::PhysicsWorld: {
                    const auto &seam = state.descriptor.collisionMode == ParticleCollisionMode::SceneDepth ? state.sceneDepth : state.physicsWorld;
                    if (seam.probe == nullptr) {
                        if (seam.required || state.requiredGameplay)
                            return Failure<CollisionSelection>(VfxErrors::ParticleCollisionQueryUnavailable);
                        return Result<CollisionSelection>::Success(selection);
                    }
                    const auto query = seam.probe(seam.context,
                                                  {.particle = handle.particle,
                                                   .previousPosition = previous,
                                                   .position = position,
                                                   .velocity = velocity,
                                                   .deltaSeconds = step.deltaSeconds,
                                                   .tick = step.tick,
                                                   .sceneGeneration = seam.sceneGeneration,
                                                   .snapshotGeneration = seam.snapshotGeneration});
                    if (query.HasError())
                        return Result<CollisionSelection>::Failure(
                            WrapError(VfxErrors::ParticleCollisionQueryFailed, query.ErrorValue()));
                    const auto &hit = query.Value();
                    if (!hit.hit)
                        return Result<CollisionSelection>::Success(selection);
                    if (!Finite(hit.position) || !Finite(hit.normal) || !Finite(hit.distance) || hit.distance < 0.0F ||
                        Math::LengthSquared(hit.normal) <= std::numeric_limits<float>::epsilon())
                        return Failure<CollisionSelection>(VfxErrors::ParticleCollisionQueryFailed);
                    selection.hit = true;
                    selection.position = hit.position;
                    selection.normal = Math::Normalize(hit.normal);
                    selection.distance = hit.distance;
                    selection.restitution = hit.restitution;
                    if (!Finite(selection.restitution) || selection.restitution < 0.0F || selection.restitution > 1.0F)
                        return Failure<CollisionSelection>(VfxErrors::ParticleCollisionQueryFailed);
                    selection.stableTarget = hit.stableTarget;
                    selection.stableFeature = hit.stableFeature;
                    return Result<CollisionSelection>::Success(selection);
                }
                case ParticleCollisionMode::Count:
                    return Failure<CollisionSelection>(VfxErrors::ParticleSimulationDescriptorInvalid);
            }
            return Failure<CollisionSelection>(VfxErrors::ParticleSimulationDescriptorInvalid);
        }

        void CompactAuxiliary(Detail::CpuParticleSimulatorState &state, const std::uint32_t survivorCount) noexcept {
            for (std::uint32_t dense = 0; dense < survivorCount; ++dense) {
                const std::uint32_t source = state.survivorSources[dense];
                const std::uint32_t destinationAuxiliary = state.candidateOffset + dense;
                const std::uint32_t sourceAuxiliary = state.candidateOffset + source;
                state.baseSizeX[destinationAuxiliary] = state.baseSizeX[sourceAuxiliary];
                state.baseSizeY[destinationAuxiliary] = state.baseSizeY[sourceAuxiliary];
                state.baseColor[destinationAuxiliary] = state.baseColor[sourceAuxiliary];
            }
        }

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
            return force.kind < CpuParticleForceKind::Count && Finite(force.vector) && Finite(force.center) && Finite(force.strength) &&
                   Finite(force.falloff) && Finite(force.frequency) && force.falloff >= 0.0F && force.frequency >= 0.0F;
        }
    }  // namespace

    /** @copydoc CpuParticleSimulator::~CpuParticleSimulator */
    CpuParticleSimulator::~CpuParticleSimulator() = default;

    /** @copydoc CpuParticleSimulator::CpuParticleSimulator */
    CpuParticleSimulator::CpuParticleSimulator(CpuParticleSimulator &&other) noexcept = default;

    /** @copydoc CpuParticleSimulator::operator= */
    CpuParticleSimulator &CpuParticleSimulator::operator=(CpuParticleSimulator &&other) noexcept = default;

    /** @copydoc CpuParticleSimulator::CpuParticleSimulator */
    CpuParticleSimulator::CpuParticleSimulator(std::unique_ptr<Detail::CpuParticleSimulatorState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc CpuParticleSimulator::Create */
    Result<CpuParticleSimulator> CpuParticleSimulator::Create(const ParticleSystemDescriptor &descriptor,
                                                              const CpuParticleSimulatorCreateInfo &info) {
        const auto &data = descriptor.Data();
        if (!info.buffer.IsValid() || !info.activation.IsValid() || !data.emitter.IsValid() || info.maximumBurstParticles == 0 ||
            info.maximumBurstParticles > CpuParticleSimulationHardLimits::BurstParticles || !Finite(info.maximumDeltaSeconds) ||
            info.maximumDeltaSeconds <= 0.0F || info.maximumDeltaSeconds > CpuParticleSimulationHardLimits::DeltaSeconds ||
            data.maximumParticles == 0 || data.maximumParticles > CpuParticleBufferHardLimits::Particles ||
            data.collisionMode >= ParticleCollisionMode::Count || info.collisionResponse >= CpuParticleCollisionResponse::Count ||
            info.forces.size() > CpuParticleSimulationHardLimits::ForceModules || info.planes.size() > CpuParticleSimulationHardLimits::Planes ||
            info.payloadChannels.size() > CpuParticleSimulationHardLimits::PayloadChannels ||
            !ValidCurve(info.sizeOverLife) || !ValidCurve(info.opacityOverLife) || !ValidColorCurve(info.colorOverLife))
            return Failure<CpuParticleSimulator>(VfxErrors::ParticleSimulationDescriptorInvalid);
        for (const auto &force : info.forces) {
            if (!ValidForce(force))
                return Failure<CpuParticleSimulator>(VfxErrors::ParticleSimulationDescriptorInvalid);
        }
        std::uint32_t customFloatStreams = info.customFloatStreams;
        for (const auto &channel : info.payloadChannels) {
            if (channel.channel == 0 || channel.classification >= CpuParticlePayloadClass::Count || !Finite(channel.minimum) ||
                !Finite(channel.maximum) || channel.minimum > channel.maximum)
                return Failure<CpuParticleSimulator>(VfxErrors::ParticlePayloadSchemaMismatch);
            for (const auto &other : info.payloadChannels) {
                if (&channel != &other && channel.channel == other.channel)
                    return Failure<CpuParticleSimulator>(VfxErrors::ParticlePayloadSchemaMismatch);
            }
            if (channel.customFloatStream == std::numeric_limits<std::uint32_t>::max())
                return Failure<CpuParticleSimulator>(VfxErrors::ParticlePayloadSchemaMismatch);
            const std::uint32_t requiredStreams = channel.customFloatStream + 1U;
            if (info.customFloatStreams != 0 && requiredStreams > info.customFloatStreams)
                return Failure<CpuParticleSimulator>(VfxErrors::ParticlePayloadSchemaMismatch);
            customFloatStreams = std::max(customFloatStreams, requiredStreams);
        }
        if (customFloatStreams > CpuParticleBufferHardLimits::CustomFloatStreams)
            return Failure<CpuParticleSimulator>(VfxErrors::ParticlePayloadSchemaMismatch);
        for (const auto &plane : info.planes) {
            if (!Finite(plane.point) || !Finite(plane.normal) || !Finite(plane.restitution) || plane.restitution < 0.0F ||
                plane.restitution > 1.0F || Math::LengthSquared(plane.normal) <= std::numeric_limits<float>::epsilon())
                return Failure<CpuParticleSimulator>(VfxErrors::ParticleSimulationDescriptorInvalid);
        }
        if (data.collisionMode == ParticleCollisionMode::Planes && info.planes.empty())
            return Failure<CpuParticleSimulator>(VfxErrors::ParticleSimulationDescriptorInvalid);
        const auto seamUnavailable = [&](const CpuParticleCollisionQuerySeam &seam) {
            return seam.required && seam.probe == nullptr;
        };
        if ((data.collisionMode == ParticleCollisionMode::SceneDepth && seamUnavailable(info.sceneDepth)) ||
            (data.collisionMode == ParticleCollisionMode::PhysicsWorld && seamUnavailable(info.physicsWorld)))
            return Failure<CpuParticleSimulator>(VfxErrors::ParticleCollisionQueryUnavailable);

        auto committed = CpuParticleBuffer::Create({.buffer = info.buffer,
                                                     .capacity = data.maximumParticles,
                                                     .customFloatStreams = customFloatStreams,
                                                     .maximumBytes = info.maximumBufferBytes});
        if (committed.HasError())
            return Result<CpuParticleSimulator>::Failure(committed.ErrorValue());
        auto candidate = CpuParticleBuffer::Create({.buffer = info.buffer,
                                                    .capacity = data.maximumParticles,
                                                    .customFloatStreams = customFloatStreams,
                                                    .maximumBytes = info.maximumBufferBytes});
        if (candidate.HasError())
            return Result<CpuParticleSimulator>::Failure(candidate.ErrorValue());

        try {
            auto state = std::make_unique<Detail::CpuParticleSimulatorState>(std::move(committed).Value(), std::move(candidate).Value());
            state->descriptor = data;
            state->buffer = info.buffer;
            state->activation = info.activation;
            state->effectSeed = info.effectSeed;
            state->ownerThread = std::this_thread::get_id();
            state->capacity = data.maximumParticles;
            state->customFloatStreams = customFloatStreams;
            state->requiredGameplay = info.requiredGameplay;
            state->maximumDeltaSeconds = info.maximumDeltaSeconds;
            state->maximumBurstParticles = info.maximumBurstParticles;
            state->sceneDepth = info.sceneDepth;
            state->physicsWorld = info.physicsWorld;
            state->collisionResponse = info.collisionResponse;
            state->stageObserver = info.stageObserver;
            state->stageObserverContext = info.stageObserverContext;
            state->forceCount = static_cast<std::uint32_t>(info.forces.size());
            std::copy(info.forces.begin(), info.forces.end(), state->forces.begin());
            state->planeCount = static_cast<std::uint32_t>(info.planes.size());
            for (std::uint32_t index = 0; index < state->planeCount; ++index) {
                state->planes[index] = info.planes[index];
                state->planes[index].normal = Math::Normalize(state->planes[index].normal);
            }
            state->sizeOverLifeCount = static_cast<std::uint32_t>(info.sizeOverLife.size());
            std::copy(info.sizeOverLife.begin(), info.sizeOverLife.end(), state->sizeOverLife.begin());
            state->opacityOverLifeCount = static_cast<std::uint32_t>(info.opacityOverLife.size());
            std::copy(info.opacityOverLife.begin(), info.opacityOverLife.end(), state->opacityOverLife.begin());
            state->colorOverLifeCount = static_cast<std::uint32_t>(info.colorOverLife.size());
            std::copy(info.colorOverLife.begin(), info.colorOverLife.end(), state->colorOverLife.begin());
            state->payloadChannelCount = static_cast<std::uint32_t>(info.payloadChannels.size());
            std::copy(info.payloadChannels.begin(), info.payloadChannels.end(), state->payloadChannels.begin());
            for (std::uint32_t index = 0; index < state->payloadChannelCount; ++index)
                state->inputValues[index] = state->payloadChannels[index].minimum;

            const std::size_t doubledCapacity = static_cast<std::size_t>(state->capacity) * 2U;
            state->handles.resize(doubledCapacity);
            state->baseSizeX.resize(doubledCapacity);
            state->baseSizeY.resize(doubledCapacity);
            state->baseColor.resize(doubledCapacity);
            state->survivorSources.resize(state->capacity);
            state->previousPositions.resize(state->capacity);
            state->acceleration.resize(state->capacity);
            state->killRequests.reserve(state->capacity);
            state->committedOffset = 0;
            state->candidateOffset = state->capacity;
            const std::uint64_t identityBase = IdentityBase(info, data);
            state->nextSimulationIdentity = identityBase == 0 ? 1 : identityBase;
            state->spawnRate = SampleRange(*state,
                                           ParticleSimulationId::Create(state->nextSimulationIdentity).Value(), 4,
                                           data.spawnRate);
            state->nextTick = 0;
            return Result<CpuParticleSimulator>::Success(CpuParticleSimulator{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<CpuParticleSimulator>(VfxErrors::ParticleBufferAllocationFailed);
        }
    }

    /** @copydoc CpuParticleSimulator::Advance */
    Result<CpuParticleSimulationStepResult> CpuParticleSimulator::Advance(const CpuParticleSimulationStep &step) {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(state.ErrorValue());
        if (step.cancelled)
            return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleSimulationStepCancelled);
        if (!Finite(step.deltaSeconds) || step.deltaSeconds < 0.0F || step.deltaSeconds > state_->maximumDeltaSeconds ||
            step.burstCount > state_->maximumBurstParticles || step.tick != state_->nextTick)
            return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleSpawnStepInvalid);
        if (state_->committedGeneration == std::numeric_limits<std::uint64_t>::max() ||
            state_->nextTick == std::numeric_limits<std::uint64_t>::max())
            return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleGenerationStale);

        if (auto copied = state_->candidate.CopyFrom(state_->committed); copied.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(copied.ErrorValue());
        const std::uint32_t currentActive = state_->committed.Statistics().active;
        std::copy_n(state_->handles.begin() + state_->committedOffset, currentActive,
                    state_->handles.begin() + state_->candidateOffset);
        std::copy_n(state_->baseSizeX.begin() + state_->committedOffset, currentActive,
                    state_->baseSizeX.begin() + state_->candidateOffset);
        std::copy_n(state_->baseSizeY.begin() + state_->committedOffset, currentActive,
                    state_->baseSizeY.begin() + state_->candidateOffset);
        std::copy_n(state_->baseColor.begin() + state_->committedOffset, currentActive,
                    state_->baseColor.begin() + state_->candidateOffset);

        if (auto observed = Observe(*state_, CpuParticleStage::Spawn); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        const auto plan = CalculateBirthPlan(*state_, step);
        if (plan.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(plan.ErrorValue());
        const std::uint32_t firstNewDense = state_->candidate.Statistics().active;
        std::uint64_t candidateNextSimulationIdentity = state_->nextSimulationIdentity;
        for (std::uint32_t birth = 0; birth < plan.Value().admitted; ++birth) {
            if (candidateNextSimulationIdentity == std::numeric_limits<std::uint64_t>::max())
                return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleSpawnOrdinalExhausted);
            auto particle = ParticleSimulationId::Create(++candidateNextSimulationIdentity);
            if (particle.HasError())
                return Result<CpuParticleSimulationStepResult>::Failure(particle.ErrorValue());
            auto spawned = state_->candidate.Spawn(particle.Value());
            if (spawned.HasError())
                return Result<CpuParticleSimulationStepResult>::Failure(spawned.ErrorValue());
            state_->handles[state_->candidateOffset + firstNewDense + birth] = spawned.Value();
        }
        const auto viewResult = state_->candidate.View();
        if (viewResult.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(viewResult.ErrorValue());
        CpuParticleSoAView view = viewResult.Value();
        for (std::uint32_t birth = 0; birth < plan.Value().admitted; ++birth) {
            const std::uint32_t dense = firstNewDense + birth;
            InitializeParticle(*state_, state_->handles[state_->candidateOffset + dense], view, dense, step.tick);
        }

        if (auto observed = Observe(*state_, CpuParticleStage::Initialize); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        if (auto observed = Observe(*state_, CpuParticleStage::Forces); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense)
            ApplyForces(*state_, view, dense, step.tick, state_->handles[state_->candidateOffset + dense].particle,
                        state_->acceleration[dense]);

        if (auto observed = Observe(*state_, CpuParticleStage::Integrate); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense) {
            state_->previousPositions[dense] = {view.positionX[dense], view.positionY[dense], view.positionZ[dense]};
            view.velocityX[dense] += state_->acceleration[dense].x * step.deltaSeconds;
            view.velocityY[dense] += state_->acceleration[dense].y * step.deltaSeconds;
            view.velocityZ[dense] += state_->acceleration[dense].z * step.deltaSeconds;
            view.positionX[dense] += view.velocityX[dense] * step.deltaSeconds;
            view.positionY[dense] += view.velocityY[dense] * step.deltaSeconds;
            view.positionZ[dense] += view.velocityZ[dense] * step.deltaSeconds;
            view.age[dense] = std::min(view.age[dense], std::numeric_limits<float>::max() - step.deltaSeconds) + step.deltaSeconds;
            view.rotation[dense] += view.angularVelocity[dense] * step.deltaSeconds;
            ApplyOverLife(*state_, view, dense);
        }

        if (auto observed = Observe(*state_, CpuParticleStage::Collide); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        std::uint32_t collisionCount{};
        for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense) {
            const auto collision = QueryCollision(*state_, step, state_->handles[state_->candidateOffset + dense],
                                                  state_->previousPositions[dense],
                                                  {view.positionX[dense], view.positionY[dense], view.positionZ[dense]},
                                                  {view.velocityX[dense], view.velocityY[dense], view.velocityZ[dense]});
            if (collision.HasError())
                return Result<CpuParticleSimulationStepResult>::Failure(collision.ErrorValue());
            if (!collision.Value().hit)
                continue;
            ++collisionCount;
            view.positionX[dense] = collision.Value().position.x + (collision.Value().normal.x * CollisionEpsilon);
            view.positionY[dense] = collision.Value().position.y + (collision.Value().normal.y * CollisionEpsilon);
            view.positionZ[dense] = collision.Value().position.z + (collision.Value().normal.z * CollisionEpsilon);
            if (state_->collisionResponse == CpuParticleCollisionResponse::Die ||
                state_->descriptor.killCondition == ParticleKillCondition::Collision) {
                view.customFlags[dense] |= ExplicitKillBit;
                continue;
            }
            const Math::Vec3 normal = collision.Value().normal;
            const Math::Vec3 velocity{view.velocityX[dense], view.velocityY[dense], view.velocityZ[dense]};
            const float normalVelocity = Math::Dot(velocity, normal);
            if (normalVelocity < 0.0F) {
                const Math::Vec3 reflected = velocity - (normal * (2.0F * normalVelocity));
                view.velocityX[dense] = reflected.x * collision.Value().restitution;
                view.velocityY[dense] = reflected.y * collision.Value().restitution;
                view.velocityZ[dense] = reflected.z * collision.Value().restitution;
            }
        }

        for (const auto &kill : state_->killRequests) {
            auto dense = state_->candidate.ResolveDenseIndex(kill);
            if (dense.HasError())
                return Result<CpuParticleSimulationStepResult>::Failure(dense.ErrorValue());
            view.customFlags[dense.Value()] |= ExplicitKillBit;
        }

        if (auto observed = Observe(*state_, CpuParticleStage::Kill); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        const std::uint32_t candidateActive = state_->candidate.Statistics().active;
        std::uint32_t survivorCount{};
        for (std::uint32_t dense = 0; dense < candidateActive; ++dense) {
            const bool expired = state_->descriptor.lifetimeKind == ParticleLifetimeKind::Finite &&
                                 view.age[dense] >= view.maximumAge[dense];
            if (expired || (view.customFlags[dense] & ExplicitKillBit) != 0)
                continue;
            state_->survivorSources[survivorCount] = dense;
            state_->handles[state_->candidateOffset + survivorCount] = state_->handles[state_->candidateOffset + dense];
            ++survivorCount;
        }
        const std::span<const CpuParticleHandle> survivors{state_->handles.data() + state_->candidateOffset, survivorCount};
        if (auto compacted = state_->candidate.CompactStable(survivors); compacted.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(compacted.ErrorValue());
        CompactAuxiliary(*state_, survivorCount);

        if (auto observed = Observe(*state_, CpuParticleStage::Extract); observed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(observed.ErrorValue());
        const std::uint32_t killedThisStep = candidateActive - survivorCount;
        std::swap(state_->committed, state_->candidate);
        std::swap(state_->committedOffset, state_->candidateOffset);
        state_->spawnCarry = plan.Value().carry;
        state_->nextSimulationIdentity = candidateNextSimulationIdentity;
        ++state_->nextTick;
        ++state_->committedGeneration;
        ++state_->steps;
        state_->spawned += plan.Value().admitted;
        state_->dropped += plan.Value().requested - plan.Value().admitted;
        state_->killed += killedThisStep;
        state_->collisions += collisionCount;
        state_->killRequests.clear();
        return Result<CpuParticleSimulationStepResult>::Success(
            {.requestedBirths = plan.Value().requested,
             .spawned = plan.Value().admitted,
             .dropped = plan.Value().requested - plan.Value().admitted,
             .killed = killedThisStep,
             .collisions = collisionCount,
             .active = survivorCount,
             .committedGeneration = state_->committedGeneration});
    }

    /** @copydoc CpuParticleSimulator::SignalKill */
    Result<void> CpuParticleSimulator::SignalKill(const CpuParticleHandle &handle) {
        if (auto state = ValidateOperationalState(state_.get()); state.HasError())
            return state;
        if (state_->killRequests.size() >= state_->capacity)
            return Failure<void>(VfxErrors::ParticleStepCapacityExceeded);
        if (auto resolved = state_->committed.ResolveDenseIndex(handle); resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        state_->killRequests.push_back(handle);
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleSimulator::HandleAtDenseIndex */
    Result<CpuParticleHandle> CpuParticleSimulator::HandleAtDenseIndex(const std::uint32_t dense) {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<CpuParticleHandle>::Failure(state.ErrorValue());
        if (dense >= state_->committed.Statistics().active)
            return Failure<CpuParticleHandle>(VfxErrors::ParticlePayloadSchemaMismatch);
        return Result<CpuParticleHandle>::Success(state_->handles[state_->committedOffset + dense]);
    }

    /** @copydoc CpuParticleSimulator::Extract */
    Result<CpuParticleExtractView> CpuParticleSimulator::Extract() {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<CpuParticleExtractView>::Failure(state.ErrorValue());
        const auto mutableView = state_->committed.View();
        if (mutableView.HasError())
            return Result<CpuParticleExtractView>::Failure(mutableView.ErrorValue());
        const auto view = mutableView.Value();
        const auto floats = [](const std::span<float> values) { return std::span<const float>{values.data(), values.size()}; };
        const auto unsigneds = [](const std::span<std::uint32_t> values) {
            return std::span<const std::uint32_t>{values.data(), values.size()};
        };
        CpuParticleExtractView result{.committedGeneration = state_->committedGeneration,
                                     .positionX = floats(view.positionX),
                                     .positionY = floats(view.positionY),
                                     .positionZ = floats(view.positionZ),
                                     .velocityX = floats(view.velocityX),
                                     .velocityY = floats(view.velocityY),
                                     .velocityZ = floats(view.velocityZ),
                                     .sizeX = floats(view.sizeX),
                                     .sizeY = floats(view.sizeY),
                                     .rotation = floats(view.rotation),
                                     .angularVelocity = floats(view.angularVelocity),
                                     .packedColor = unsigneds(view.packedColor),
                                     .age = floats(view.age),
                                     .maximumAge = floats(view.maximumAge),
                                     .customFlags = unsigneds(view.customFlags),
                                     .customFloatStreamCount = state_->customFloatStreams};
        for (std::uint32_t index = 0; index < state_->customFloatStreams; ++index)
            result.customFloats[index] = floats(view.customFloats[index]);
        return Result<CpuParticleExtractView>::Success(result);
    }

    /** @copydoc CpuParticleSimulator::SubmitGameplayInput */
    Result<void> CpuParticleSimulator::SubmitGameplayInput(const std::uint16_t channel, const float value) {
        if (auto state = ValidateOperationalState(state_.get()); state.HasError())
            return state;
        if (!Finite(value))
            return Failure<void>(VfxErrors::ParticlePayloadSchemaMismatch);
        for (std::uint32_t index = 0; index < state_->payloadChannelCount; ++index) {
            const auto &schema = state_->payloadChannels[index];
            if (schema.channel != channel)
                continue;
            if (schema.classification != CpuParticlePayloadClass::GameplayInput)
                return Failure<void>(VfxErrors::ParticleGameplayAccessDenied);
            if (value < schema.minimum || value > schema.maximum)
                return Failure<void>(VfxErrors::ParticlePayloadSchemaMismatch);
            state_->inputValues[index] = value;
            return Result<void>::Success();
        }
        return Failure<void>(VfxErrors::ParticlePayloadSchemaMismatch);
    }

    /** @copydoc CpuParticleSimulator::ReadGameplayOutput */
    Result<float> CpuParticleSimulator::ReadGameplayOutput(const std::uint16_t channel, const std::uint32_t dense,
                                                            const std::uint64_t committedGeneration) const {
        if (state_ == nullptr || state_->shutDown)
            return Failure<float>(VfxErrors::ParticleBufferShutDown);
        if (const auto owner = OwnerThreadResult(*state_); owner.HasError())
            return Result<float>::Failure(owner.ErrorValue());
        if (committedGeneration == 0 || committedGeneration != state_->committedGeneration)
            return Failure<float>(VfxErrors::ParticleGenerationStale);
        const CpuParticlePayloadChannel *schema = nullptr;
        for (std::uint32_t index = 0; index < state_->payloadChannelCount; ++index) {
            if (state_->payloadChannels[index].channel == channel) {
                schema = &state_->payloadChannels[index];
                break;
            }
        }
        if (schema == nullptr)
            return Failure<float>(VfxErrors::ParticlePayloadSchemaMismatch);
        if (schema->classification != CpuParticlePayloadClass::GameplayOutput)
            return Failure<float>(VfxErrors::ParticleGameplayAccessDenied);
        const std::uint32_t active = state_->committed.Statistics().active;
        if (dense >= active)
            return Failure<float>(VfxErrors::ParticlePayloadSchemaMismatch);
        auto &buffer = const_cast<CpuParticleBuffer &>(state_->committed);
        const auto view = buffer.View();
        if (view.HasError())
            return Result<float>::Failure(view.ErrorValue());
        return Result<float>::Success(view.Value().customFloats[schema->customFloatStream][dense]);
    }

    /** @copydoc CpuParticleSimulator::Statistics */
    CpuParticleSimulationStatistics CpuParticleSimulator::Statistics() const noexcept {
        if (state_ == nullptr)
            return {};
        return {.committedGeneration = state_->committedGeneration,
                .steps = state_->steps,
                .spawned = state_->spawned,
                .dropped = state_->dropped,
                .killed = state_->killed,
                .collisions = state_->collisions,
                .nextSpawnOrdinal = state_->nextSimulationIdentity,
                .active = state_->committed.Statistics().active};
    }

    /** @copydoc CpuParticleSimulator::Shutdown */
    Result<void> CpuParticleSimulator::Shutdown() {
        if (state_ == nullptr)
            return Result<void>::Success();
        if (const auto owner = OwnerThreadResult(*state_); owner.HasError())
            return owner;
        if (!state_->shutDown) {
            if (auto result = state_->committed.Shutdown(); result.HasError())
                return result;
            if (auto result = state_->candidate.Shutdown(); result.HasError())
                return result;
            state_->shutDown = true;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Vfx
