#include "CpuParticleSimulatorInternal.h"
#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Vfx::CpuParticleSimulatorDetail {
    using Detail::CpuParticleCollisionSelection;
    using Detail::CpuParticleSpawnStageResult;

    namespace {
        constexpr std::uint32_t ExplicitKillBit = 1U << 0U;
        constexpr float Tau = 6.28318530717958647692F;
        constexpr float CollisionEpsilon = 0.0001F;

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

        void InitializeParticle(Detail::CpuParticleSimulatorState &state, const CpuParticleHandle &handle, const CpuParticleSoAView &view,
                                const std::uint32_t dense, const std::uint64_t tick) noexcept {
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
                                          const std::uint32_t count, const float normalizedAge, const float fallback) noexcept {
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

        [[nodiscard]] Math::Vec4 EvaluateColorCurve(const std::array<CpuParticleColorKey, CpuParticleSimulationHardLimits::CurveKeys> &keys,
                                                    const std::uint32_t count, const float normalizedAge,
                                                    const Math::Vec4 fallback) noexcept {
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

        void ApplyOverLife(Detail::CpuParticleSimulatorState &state, const CpuParticleSoAView &view, const std::uint32_t dense) noexcept {
            const float maximumAge = view.maximumAge[dense];
            const float normalizedAge = maximumAge > 0.0F && Finite(maximumAge) ? ClampUnit(view.age[dense] / maximumAge) : 0.0F;
            const std::uint32_t auxiliary = state.candidateOffset + dense;
            const float sizeMultiplier = EvaluateCurve(state.sizeOverLife, state.sizeOverLifeCount, normalizedAge, 1.0F);
            view.sizeX[dense] = state.baseSizeX[auxiliary] * sizeMultiplier;
            view.sizeY[dense] = state.baseSizeY[auxiliary] * sizeMultiplier;
            const float opacityMultiplier = EvaluateCurve(state.opacityOverLife, state.opacityOverLifeCount, normalizedAge, 1.0F);
            Math::Vec4 color = EvaluateColorCurve(state.colorOverLife, state.colorOverLifeCount, normalizedAge, state.baseColor[auxiliary]);
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

        /** @brief Returns zero at the attraction center and otherwise applies radial attenuation. */
        [[nodiscard]] Math::Vec3 AttractionContribution(const CpuParticleForceModule &force, const Math::Vec3 position) noexcept {
            const Math::Vec3 delta = force.center - position;
            const float lengthSquared = Math::LengthSquared(delta);
            if (lengthSquared <= std::numeric_limits<float>::epsilon())
                return {};
            const float length = std::sqrt(lengthSquared);
            const float attenuation = 1.0F / (1.0F + (force.falloff * length));
            return (delta / length) * (force.strength * attenuation);
        }

        [[nodiscard]] Result<CpuParticleCollisionSelection> QueryPlanes(const Detail::CpuParticleSimulatorState &state,
                                                                        const Math::Vec3 previous, const Math::Vec3 position) {
            CpuParticleCollisionSelection selection{};
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
            return Result<CpuParticleCollisionSelection>::Success(selection);
        }

        [[nodiscard]] Result<CpuParticleCollisionSelection> QueryAdapter(const Detail::CpuParticleSimulatorState &state,
                                                                         const CpuParticleSimulationStep &step,
                                                                         const CpuParticleCollisionQuerySeam &seam,
                                                                         const CpuParticleHandle &handle, const Math::Vec3 previous,
                                                                         const Math::Vec3 position, const Math::Vec3 velocity) {
            CpuParticleCollisionSelection selection{};
            if (seam.probe == nullptr) {
                if (seam.required || state.requiredGameplay)
                    return Failure<CpuParticleCollisionSelection>(VfxErrors::ParticleCollisionQueryUnavailable);
                return Result<CpuParticleCollisionSelection>::Success(selection);
            }
            const auto query = seam.probe(seam.context, {.particle = handle.particle,
                                                         .previousPosition = previous,
                                                         .position = position,
                                                         .velocity = velocity,
                                                         .deltaSeconds = step.deltaSeconds,
                                                         .tick = step.tick,
                                                         .sceneGeneration = seam.sceneGeneration,
                                                         .snapshotGeneration = seam.snapshotGeneration});
            if (query.HasError())
                return Result<CpuParticleCollisionSelection>::Failure(
                    WrapError(VfxErrors::ParticleCollisionQueryFailed, query.ErrorValue()));
            const auto &hit = query.Value();
            if (!hit.hit)
                return Result<CpuParticleCollisionSelection>::Success(selection);
            if (!Finite(hit.position) || !Finite(hit.normal) || !Finite(hit.distance) || hit.distance < 0.0F ||
                Math::LengthSquared(hit.normal) <= std::numeric_limits<float>::epsilon() || !Finite(hit.restitution) ||
                hit.restitution < 0.0F || hit.restitution > 1.0F)
                return Failure<CpuParticleCollisionSelection>(VfxErrors::ParticleCollisionQueryFailed);
            selection.hit = true;
            selection.position = hit.position;
            selection.normal = Math::Normalize(hit.normal);
            selection.distance = hit.distance;
            selection.restitution = hit.restitution;
            selection.stableTarget = hit.stableTarget;
            selection.stableFeature = hit.stableFeature;
            return Result<CpuParticleCollisionSelection>::Success(selection);
        }

        [[nodiscard]] Result<CpuParticleCollisionSelection> QueryCollision(const Detail::CpuParticleSimulatorState &state,
                                                                           const CpuParticleSimulationStep &step,
                                                                           const CpuParticleHandle &handle, const Math::Vec3 previous,
                                                                           const Math::Vec3 position, const Math::Vec3 velocity) {
            switch (state.descriptor.collisionMode) {
                case ParticleCollisionMode::None:
                    return Result<CpuParticleCollisionSelection>::Success({});
                case ParticleCollisionMode::Planes:
                    return QueryPlanes(state, previous, position);
                case ParticleCollisionMode::SceneDepth:
                    return QueryAdapter(state, step, state.sceneDepth, handle, previous, position, velocity);
                case ParticleCollisionMode::PhysicsWorld:
                    return QueryAdapter(state, step, state.physicsWorld, handle, previous, position, velocity);
                case ParticleCollisionMode::Count:
                    return Failure<CpuParticleCollisionSelection>(VfxErrors::ParticleSimulationDescriptorInvalid);
            }
            return Failure<CpuParticleCollisionSelection>(VfxErrors::ParticleSimulationDescriptorInvalid);
        }

        [[nodiscard]] Result<Detail::CpuParticleBirthPlan> CalculateBirthPlan(const Detail::CpuParticleSimulatorState &state,
                                                                              const CpuParticleSimulationStep &step) {
            const double continuous = state.spawnCarry + (static_cast<double>(state.spawnRate) * step.deltaSeconds);
            if (!std::isfinite(continuous) || continuous < 0.0)
                return Failure<Detail::CpuParticleBirthPlan>(VfxErrors::ParticleSimulationDescriptorInvalid);
            const double integral = std::floor(continuous);
            const auto continuousBirths = integral >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                                              ? std::numeric_limits<std::uint64_t>::max()
                                              : static_cast<std::uint64_t>(integral);
            if (continuousBirths > std::numeric_limits<std::uint64_t>::max() - step.burstCount)
                return Failure<Detail::CpuParticleBirthPlan>(VfxErrors::ParticleSimulationDescriptorInvalid);
            const std::uint64_t requested = continuousBirths + step.burstCount;
            const std::uint32_t available = state.candidate.Statistics().available;
            if (state.requiredGameplay && requested > available)
                return Failure<Detail::CpuParticleBirthPlan>(VfxErrors::ParticleStepCapacityExceeded);
            const auto admitted = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, available));
            if (state.nextSimulationIdentity > std::numeric_limits<std::uint64_t>::max() - admitted)
                return Failure<Detail::CpuParticleBirthPlan>(VfxErrors::ParticleSpawnOrdinalExhausted);
            return Result<Detail::CpuParticleBirthPlan>::Success(
                {.requested = requested, .admitted = admitted, .carry = continuous - integral});
        }
    }  // namespace

    Result<void> PrepareCandidate(Detail::CpuParticleSimulatorState &state) {
        if (auto copied = state.candidate.CopyFrom(state.committed); copied.HasError())
            return copied;
        const std::uint32_t active = state.committed.Statistics().active;
        std::copy_n(state.handles.begin() + state.committedOffset, active, state.handles.begin() + state.candidateOffset);
        std::copy_n(state.baseSizeX.begin() + state.committedOffset, active, state.baseSizeX.begin() + state.candidateOffset);
        std::copy_n(state.baseSizeY.begin() + state.committedOffset, active, state.baseSizeY.begin() + state.candidateOffset);
        std::copy_n(state.baseColor.begin() + state.committedOffset, active, state.baseColor.begin() + state.candidateOffset);
        return Result<void>::Success();
    }

    Result<CpuParticleSpawnStageResult> RunSpawnStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step) {
        if (auto observed = Observe(state, CpuParticleStage::Spawn); observed.HasError())
            return Result<CpuParticleSpawnStageResult>::Failure(observed.ErrorValue());
        const auto plan = CalculateBirthPlan(state, step);
        if (plan.HasError())
            return Result<CpuParticleSpawnStageResult>::Failure(plan.ErrorValue());
        std::uint64_t nextIdentity = state.nextSimulationIdentity;
        const std::uint32_t firstDense = state.candidate.Statistics().active;
        for (std::uint32_t birth = 0; birth < plan.Value().admitted; ++birth) {
            if (nextIdentity == std::numeric_limits<std::uint64_t>::max())
                return Failure<CpuParticleSpawnStageResult>(VfxErrors::ParticleSpawnOrdinalExhausted);
            auto particle = ParticleSimulationId::Create(++nextIdentity);
            if (particle.HasError())
                return Result<CpuParticleSpawnStageResult>::Failure(particle.ErrorValue());
            auto spawned = state.candidate.Spawn(particle.Value());
            if (spawned.HasError())
                return Result<CpuParticleSpawnStageResult>::Failure(spawned.ErrorValue());
            state.handles[state.candidateOffset + firstDense + birth] = spawned.Value();
        }
        return Result<CpuParticleSpawnStageResult>::Success({.plan = plan.Value(), .nextSimulationIdentity = nextIdentity});
    }

    Result<void> RunInitializeStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step,
                                    const Detail::CpuParticleSpawnStageResult &spawn) {
        if (auto observed = Observe(state, CpuParticleStage::Initialize); observed.HasError())
            return observed;
        const auto viewResult = state.candidate.View();
        if (viewResult.HasError())
            return Result<void>::Failure(viewResult.ErrorValue());
        const auto view = viewResult.Value();
        const std::uint32_t firstDense = state.candidate.Statistics().active - spawn.plan.admitted;
        for (std::uint32_t birth = 0; birth < spawn.plan.admitted; ++birth) {
            const std::uint32_t dense = firstDense + birth;
            InitializeParticle(state, state.handles[state.candidateOffset + dense], view, dense, step.tick);
        }
        return Result<void>::Success();
    }

    Result<void> RunForcesStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step) {
        if (auto observed = Observe(state, CpuParticleStage::Forces); observed.HasError())
            return observed;
        const auto viewResult = state.candidate.View();
        if (viewResult.HasError())
            return Result<void>::Failure(viewResult.ErrorValue());
        const auto view = viewResult.Value();
        std::fill_n(state.acceleration.begin(), view.positionX.size(), Math::Vec3{});
        // Descriptor order is observable for floating-point accumulation. Dispatch once per
        // module; every particle sees the same ordered stack without a per-particle kind test.
        for (std::uint32_t forceIndex = 0; forceIndex < state.forceCount; ++forceIndex) {
            const auto &force = state.forces[forceIndex];
            switch (force.kind) {
                case CpuParticleForceKind::Gravity:
                case CpuParticleForceKind::Wind: {
                    const Math::Vec3 contribution = force.vector * force.strength;
                    for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense)
                        state.acceleration[dense] += contribution;
                    break;
                }
                case CpuParticleForceKind::Attraction:
                    for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense) {
                        const Math::Vec3 position{view.positionX[dense], view.positionY[dense], view.positionZ[dense]};
                        state.acceleration[dense] += AttractionContribution(force, position);
                    }
                    break;
                case CpuParticleForceKind::Noise:
                    for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense) {
                        const auto particle = state.handles[state.candidateOffset + dense].particle;
                        const float x = (UnitFloat(state, particle, force.randomChannel, 0, step.tick) * 2.0F) - 1.0F;
                        const float y = (UnitFloat(state, particle, force.randomChannel, 1, step.tick) * 2.0F) - 1.0F;
                        const float z = (UnitFloat(state, particle, force.randomChannel, 2, step.tick) * 2.0F) - 1.0F;
                        state.acceleration[dense] += Math::Vec3{x, y, z} * (force.strength * force.frequency);
                    }
                    break;
                case CpuParticleForceKind::Count:
                    break;
            }
        }
        return Result<void>::Success();
    }

    Result<void> RunIntegrateStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step) {
        if (auto observed = Observe(state, CpuParticleStage::Integrate); observed.HasError())
            return observed;
        const auto viewResult = state.candidate.View();
        if (viewResult.HasError())
            return Result<void>::Failure(viewResult.ErrorValue());
        const auto view = viewResult.Value();
        for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense) {
            state.previousPositions[dense] = {view.positionX[dense], view.positionY[dense], view.positionZ[dense]};
            view.velocityX[dense] += state.acceleration[dense].x * step.deltaSeconds;
            view.velocityY[dense] += state.acceleration[dense].y * step.deltaSeconds;
            view.velocityZ[dense] += state.acceleration[dense].z * step.deltaSeconds;
            view.positionX[dense] += view.velocityX[dense] * step.deltaSeconds;
            view.positionY[dense] += view.velocityY[dense] * step.deltaSeconds;
            view.positionZ[dense] += view.velocityZ[dense] * step.deltaSeconds;
            view.age[dense] = std::min(view.age[dense], std::numeric_limits<float>::max() - step.deltaSeconds) + step.deltaSeconds;
            view.rotation[dense] += view.angularVelocity[dense] * step.deltaSeconds;
            ApplyOverLife(state, view, dense);
        }
        return Result<void>::Success();
    }

    Result<std::uint32_t> RunCollideStage(Detail::CpuParticleSimulatorState &state, const CpuParticleSimulationStep &step) {
        if (auto observed = Observe(state, CpuParticleStage::Collide); observed.HasError())
            return Result<std::uint32_t>::Failure(observed.ErrorValue());
        const auto viewResult = state.candidate.View();
        if (viewResult.HasError())
            return Result<std::uint32_t>::Failure(viewResult.ErrorValue());
        const auto view = viewResult.Value();
        std::uint32_t collisions{};
        for (std::uint32_t dense = 0; dense < view.positionX.size(); ++dense) {
            const auto collision = QueryCollision(state, step, state.handles[state.candidateOffset + dense], state.previousPositions[dense],
                                                  {view.positionX[dense], view.positionY[dense], view.positionZ[dense]},
                                                  {view.velocityX[dense], view.velocityY[dense], view.velocityZ[dense]});
            if (collision.HasError())
                return Result<std::uint32_t>::Failure(collision.ErrorValue());
            if (!collision.Value().hit)
                continue;
            ++collisions;
            view.positionX[dense] = collision.Value().position.x + (collision.Value().normal.x * CollisionEpsilon);
            view.positionY[dense] = collision.Value().position.y + (collision.Value().normal.y * CollisionEpsilon);
            view.positionZ[dense] = collision.Value().position.z + (collision.Value().normal.z * CollisionEpsilon);
            if (state.collisionResponse == CpuParticleCollisionResponse::Die ||
                state.descriptor.killCondition == ParticleKillCondition::Collision) {
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
        return Result<std::uint32_t>::Success(collisions);
    }

    void CommitAuxiliary(Detail::CpuParticleSimulatorState &state, const std::uint32_t survivorCount) noexcept {
        for (std::uint32_t dense = 0; dense < survivorCount; ++dense) {
            const std::uint32_t source = state.survivorSources[dense];
            const std::uint32_t destinationAuxiliary = state.candidateOffset + dense;
            const std::uint32_t sourceAuxiliary = state.candidateOffset + source;
            state.baseSizeX[destinationAuxiliary] = state.baseSizeX[sourceAuxiliary];
            state.baseSizeY[destinationAuxiliary] = state.baseSizeY[sourceAuxiliary];
            state.baseColor[destinationAuxiliary] = state.baseColor[sourceAuxiliary];
        }
    }

    Result<std::uint32_t> RunKillStage(Detail::CpuParticleSimulatorState &state) {
        const auto viewResult = state.candidate.View();
        if (viewResult.HasError())
            return Result<std::uint32_t>::Failure(viewResult.ErrorValue());
        const auto view = viewResult.Value();
        for (const auto &kill : state.killRequests) {
            auto dense = state.candidate.ResolveDenseIndex(kill);
            if (dense.HasError())
                return Result<std::uint32_t>::Failure(dense.ErrorValue());
            view.customFlags[dense.Value()] |= ExplicitKillBit;
        }
        if (auto observed = Observe(state, CpuParticleStage::Kill); observed.HasError())
            return Result<std::uint32_t>::Failure(observed.ErrorValue());
        const std::uint32_t active = state.candidate.Statistics().active;
        std::uint32_t survivorCount{};
        for (std::uint32_t dense = 0; dense < active; ++dense) {
            const bool expired = state.descriptor.lifetimeKind == ParticleLifetimeKind::Finite && view.age[dense] >= view.maximumAge[dense];
            if (expired || (view.customFlags[dense] & ExplicitKillBit) != 0)
                continue;
            state.survivorSources[survivorCount] = dense;
            state.handles[state.candidateOffset + survivorCount] = state.handles[state.candidateOffset + dense];
            ++survivorCount;
        }
        const std::span<const CpuParticleHandle> survivors{state.handles.data() + state.candidateOffset, survivorCount};
        if (auto compacted = state.candidate.CompactStable(survivors); compacted.HasError())
            return Result<std::uint32_t>::Failure(compacted.ErrorValue());
        CommitAuxiliary(state, survivorCount);
        return Result<std::uint32_t>::Success(active - survivorCount);
    }
}  // namespace Horo::Vfx::CpuParticleSimulatorDetail
