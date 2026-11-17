#include "Horo/Vfx/CpuParticleSpawnPipeline.h"

#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Vfx {
    namespace Detail {
        struct CpuParticleSpawnPipelineState final {
            CpuParticleSpawnPipelineState(CpuParticleBuffer bufferValue, const ParticleSystemDescriptorData &descriptorValue,
                                          const CpuParticleSpawnPipelineCreateInfo &infoValue,
                                          std::vector<CpuParticleHandle> handlesValue) noexcept
                : buffer(std::move(bufferValue)), descriptor(descriptorValue), info(infoValue), handles(std::move(handlesValue)) {}

            CpuParticleBuffer buffer;
            ParticleSystemDescriptorData descriptor;
            CpuParticleSpawnPipelineCreateInfo info;
            std::vector<CpuParticleHandle> handles;
            double spawnRate{};
            double spawnCarry{};
            std::uint64_t nextSimulationIdentity{};
            CpuParticleSpawnStatistics statistics{};
            bool shutDown{};
        };
    }  // namespace Detail

    namespace {
        constexpr double Tau = 6.283185307179586476925286766559;
        constexpr std::uint32_t ExplicitKillBit = 1U << 0U;

        struct BirthPlan final {
            std::uint64_t requested{};
            std::uint32_t admitted{};
            double carry{};
        };

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] std::uint64_t Mix(std::uint64_t value) noexcept {
            value += 0x9E3779B97F4A7C15ULL;
            value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        }

        void HashCombine(std::uint64_t &hash, const std::uint64_t value) noexcept {
            hash = Mix(hash ^ Mix(value));
        }

        [[nodiscard]] std::uint64_t IdentityBase(const EffectSystemId activation, const EmitterId emitter) noexcept {
            std::uint64_t hash = 0x5646585041525431ULL;
            HashCombine(hash, activation.scope.Value());
            HashCombine(hash, activation.slot);
            HashCombine(hash, activation.generation);
            HashCombine(hash, emitter.scope.Value());
            HashCombine(hash, emitter.slot);
            HashCombine(hash, emitter.generation);
            return hash & 0x0000FFFFFFFFFFFFULL;
        }

        [[nodiscard]] std::uint64_t RandomWord(const Detail::CpuParticleSpawnPipelineState &state, const ParticleSimulationId particle,
                                               const ParticleRandomChannel channel, const std::uint32_t sampleOrdinal = 0) noexcept {
            std::uint64_t hash = Mix(state.info.effectSeed);
            HashCombine(hash, state.info.activation.scope.Value());
            HashCombine(hash, state.info.activation.slot);
            HashCombine(hash, state.info.activation.generation);
            HashCombine(hash, state.descriptor.emitter.scope.Value());
            HashCombine(hash, state.descriptor.emitter.slot);
            HashCombine(hash, state.descriptor.emitter.generation);
            HashCombine(hash, particle.Value());
            HashCombine(hash, static_cast<std::uint8_t>(channel));
            HashCombine(hash, sampleOrdinal);
            HashCombine(hash, CpuParticleSpawnHardLimits::RandomAlgorithmVersion);
            return hash;
        }

        [[nodiscard]] float UnitFloat(const Detail::CpuParticleSpawnPipelineState &state, const ParticleSimulationId particle,
                                      const ParticleRandomChannel channel, const std::uint32_t sampleOrdinal = 0) noexcept {
            constexpr float Scale = 1.0F / 16'777'216.0F;
            return static_cast<float>(RandomWord(state, particle, channel, sampleOrdinal) >> 40U) * Scale;
        }

        [[nodiscard]] float SampleRange(const Detail::CpuParticleSpawnPipelineState &state, const ParticleSimulationId particle,
                                        const ParticleRandomChannel channel, const ParticleScalarRange range) noexcept {
            const double unit = UnitFloat(state, particle, channel);
            return static_cast<float>(range.minimum + ((range.maximum - range.minimum) * unit));
        }

        [[nodiscard]] bool FloatRangeRepresentable(const ParticleScalarRange range) noexcept {
            constexpr double FloatMaximum = std::numeric_limits<float>::max();
            return range.minimum >= -FloatMaximum && range.maximum <= FloatMaximum;
        }

        /** @brief Checks the legacy spawn owner's finite work envelope before allocation. */
        [[nodiscard]] bool ValidSpawnOwner(const CpuParticleSpawnPipelineCreateInfo &info) noexcept {
            return info.buffer.IsValid() && info.activation.IsValid() && info.maximumBurstParticles != 0 &&
                   info.maximumBurstParticles <= CpuParticleSpawnHardLimits::BurstParticles && std::isfinite(info.maximumDeltaSeconds) &&
                   info.maximumDeltaSeconds > 0.0F && info.maximumDeltaSeconds <= CpuParticleSpawnHardLimits::DeltaSeconds;
        }

        /** @brief Requires all authored spawn ranges to fit the CPU storage representation. */
        [[nodiscard]] bool ValidSpawnRanges(const ParticleSystemDescriptorData &data) noexcept {
            return data.maximumParticles != 0 && FloatRangeRepresentable(data.initialSpeed) && FloatRangeRepresentable(data.initialSize) &&
                   FloatRangeRepresentable(data.initialOpacity) && FloatRangeRepresentable(data.lifetimeSeconds);
        }

        void InitializePosition(const Detail::CpuParticleSpawnPipelineState &state, const ParticleSimulationId particle,
                                const CpuParticleSoAView &view, const std::uint32_t dense) noexcept {
            using enum ParticleEmitterShape;

            const double u = UnitFloat(state, particle, ParticleRandomChannel::SpawnX);
            const double v = UnitFloat(state, particle, ParticleRandomChannel::SpawnY);
            const double w = UnitFloat(state, particle, ParticleRandomChannel::SpawnZ);
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
                    const double angle = Tau * u;
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
                    const double angle = Tau * u;
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

        void InitializeVelocity(const Detail::CpuParticleSpawnPipelineState &state, const ParticleSimulationId particle,
                                const CpuParticleSoAView &view, const std::uint32_t dense) noexcept {
            double x = view.positionX[dense];
            double y = view.positionY[dense];
            double z = view.positionZ[dense];
            double length = std::sqrt((x * x) + (y * y) + (z * z));
            if (length <= std::numeric_limits<double>::epsilon()) {
                const double cosine = (2.0 * UnitFloat(state, particle, ParticleRandomChannel::SpawnY, 1)) - 1.0;
                const double angle = Tau * UnitFloat(state, particle, ParticleRandomChannel::SpawnX, 1);
                const double radial = std::sqrt(std::max(0.0, 1.0 - (cosine * cosine)));
                x = radial * std::cos(angle);
                y = radial * std::sin(angle);
                z = cosine;
                length = 1.0;
            }
            const double speed = SampleRange(state, particle, ParticleRandomChannel::Speed, state.descriptor.initialSpeed);
            view.velocityX[dense] = static_cast<float>((x / length) * speed);
            view.velocityY[dense] = static_cast<float>((y / length) * speed);
            view.velocityZ[dense] = static_cast<float>((z / length) * speed);
        }

        void InitializeParticle(const Detail::CpuParticleSpawnPipelineState &state, const CpuParticleHandle &handle,
                                const CpuParticleSoAView &view, const std::uint32_t dense) noexcept {
            using enum ParticleRandomChannel;

            InitializePosition(state, handle.particle, view, dense);
            InitializeVelocity(state, handle.particle, view, dense);
            const float size = SampleRange(state, handle.particle, Size, state.descriptor.initialSize);
            const float opacity = SampleRange(state, handle.particle, Opacity, state.descriptor.initialOpacity);
            view.sizeX[dense] = size;
            view.sizeY[dense] = size;
            view.rotation[dense] = static_cast<float>(Tau * UnitFloat(state, handle.particle, Rotation));
            view.angularVelocity[dense] = 0.0F;
            view.age[dense] = 0.0F;
            view.maximumAge[dense] = state.descriptor.lifetimeKind == ParticleLifetimeKind::Finite
                                         ? SampleRange(state, handle.particle, Lifetime, state.descriptor.lifetimeSeconds)
                                         : std::numeric_limits<float>::max();
            view.customFlags[dense] = 0U;
            const auto alpha = static_cast<std::uint32_t>(std::clamp(opacity, 0.0F, 1.0F) * 255.0F + 0.5F);
            view.packedColor[dense] = 0xFFFFFF00U | alpha;
        }

        [[nodiscard]] bool ShouldKill(const Detail::CpuParticleSpawnPipelineState &state, const CpuParticleSoAView &view,
                                      const std::uint32_t dense) noexcept {
            if ((view.customFlags[dense] & ExplicitKillBit) != 0)
                return true;
            return state.descriptor.lifetimeKind == ParticleLifetimeKind::Finite && view.age[dense] >= view.maximumAge[dense];
        }

        [[nodiscard]] Result<BirthPlan> CalculateBirthPlan(const Detail::CpuParticleSpawnPipelineState &state,
                                                           const CpuParticleSpawnStep &step) {
            const double continuous = state.spawnCarry + (state.spawnRate * step.deltaSeconds);
            const double integral = std::floor(continuous);
            const auto continuousBirths = integral >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                                              ? std::numeric_limits<std::uint64_t>::max()
                                              : static_cast<std::uint64_t>(integral);
            const std::uint64_t requested = continuousBirths > std::numeric_limits<std::uint64_t>::max() - step.burstCount
                                                ? std::numeric_limits<std::uint64_t>::max()
                                                : continuousBirths + step.burstCount;
            const std::uint32_t available = state.buffer.Statistics().available;
            const auto admitted = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, available));
            if (state.nextSimulationIdentity > std::numeric_limits<std::uint64_t>::max() - admitted)
                return Failure<BirthPlan>(VfxErrors::ParticleSpawnOrdinalExhausted);
            return Result<BirthPlan>::Success({.requested = requested, .admitted = admitted, .carry = continuous - integral});
        }

        [[nodiscard]] Result<std::uint32_t> SpawnParticles(Detail::CpuParticleSpawnPipelineState &state, const std::uint32_t admitted) {
            const auto firstDense = state.buffer.Statistics().active;
            for (std::uint32_t birth = 0; birth < admitted; ++birth) {
                auto particle = ParticleSimulationId::Create(++state.nextSimulationIdentity);
                if (particle.HasError())
                    return Failure<std::uint32_t>(VfxErrors::ParticleSpawnOrdinalExhausted);
                auto spawned = state.buffer.Spawn(particle.Value());
                if (spawned.HasError())
                    return Result<std::uint32_t>::Failure(spawned.ErrorValue());
                state.handles[firstDense + birth] = spawned.Value();
            }
            return Result<std::uint32_t>::Success(firstDense);
        }

        void InitializeParticles(const Detail::CpuParticleSpawnPipelineState &state, const CpuParticleSoAView &view,
                                 const std::uint32_t firstDense, const std::uint32_t admitted) noexcept {
            for (std::uint32_t birth = 0; birth < admitted; ++birth) {
                const std::uint32_t dense = firstDense + birth;
                InitializeParticle(state, state.handles[dense], view, dense);
            }
        }

        void AdvanceAges(const CpuParticleSoAView &view, const float deltaSeconds) noexcept {
            for (float &age : view.age)
                age = std::min(age, std::numeric_limits<float>::max() - deltaSeconds) + deltaSeconds;
        }

        [[nodiscard]] Result<std::uint32_t> ReclaimParticles(Detail::CpuParticleSpawnPipelineState &state, const CpuParticleSoAView &view) {
            std::uint32_t killed{};
            std::uint32_t dense{};
            std::uint32_t active = state.buffer.Statistics().active;
            while (dense < active) {
                if (!ShouldKill(state, view, dense)) {
                    ++dense;
                    continue;
                }
                const std::uint32_t last = active - 1U;
                const CpuParticleHandle handle = state.handles[dense];
                if (auto result = state.buffer.Kill(handle); result.HasError())
                    return Result<std::uint32_t>::Failure(result.ErrorValue());
                state.handles[dense] = state.handles[last];
                state.handles[last] = {};
                --active;
                ++killed;
            }
            return Result<std::uint32_t>::Success(killed);
        }
    }  // namespace

    /** @copydoc CpuParticleSpawnPipeline::~CpuParticleSpawnPipeline */
    CpuParticleSpawnPipeline::~CpuParticleSpawnPipeline() = default;

    /** @copydoc CpuParticleSpawnPipeline::CpuParticleSpawnPipeline */
    CpuParticleSpawnPipeline::CpuParticleSpawnPipeline(CpuParticleSpawnPipeline &&other) noexcept = default;

    /** @copydoc CpuParticleSpawnPipeline::operator= */
    CpuParticleSpawnPipeline &CpuParticleSpawnPipeline::operator=(CpuParticleSpawnPipeline &&other) noexcept = default;

    /** @copydoc CpuParticleSpawnPipeline::CpuParticleSpawnPipeline */
    CpuParticleSpawnPipeline::CpuParticleSpawnPipeline(std::unique_ptr<Detail::CpuParticleSpawnPipelineState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc CpuParticleSpawnPipeline::Create */
    Result<CpuParticleSpawnPipeline> CpuParticleSpawnPipeline::Create(const ParticleSystemDescriptor &descriptor,
                                                                      const CpuParticleSpawnPipelineCreateInfo &info) {
        const ParticleSystemDescriptorData &data = descriptor.Data();
        if (!ValidSpawnOwner(info) || !ValidSpawnRanges(data))
            return Failure<CpuParticleSpawnPipeline>(VfxErrors::ParticleSpawnStepInvalid);
        if (data.gpuReadback.enabled)
            return Failure<CpuParticleSpawnPipeline>(VfxErrors::GpuSimulationUnsupported);

        auto buffer = CpuParticleBuffer::Create(
            {.buffer = info.buffer, .capacity = data.maximumParticles, .customFloatStreams = 0, .maximumBytes = info.maximumBufferBytes});
        if (buffer.HasError())
            return Result<CpuParticleSpawnPipeline>::Failure(buffer.ErrorValue());

        try {
            std::vector<CpuParticleHandle> handles(data.maximumParticles);
            auto state = std::make_unique<Detail::CpuParticleSpawnPipelineState>(std::move(buffer).Value(), data, info, std::move(handles));
            const std::uint64_t base = IdentityBase(info.activation, data.emitter);
            state->nextSimulationIdentity = base == 0 ? 1 : base;
            const auto rateParticle = ParticleSimulationId::Create(state->nextSimulationIdentity).Value();
            state->spawnRate = SampleRange(*state, rateParticle, ParticleRandomChannel::SpawnRate, data.spawnRate);
            return Result<CpuParticleSpawnPipeline>::Success(CpuParticleSpawnPipeline{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<CpuParticleSpawnPipeline>(VfxErrors::ParticleBufferAllocationFailed);
        }
    }

    /** @copydoc CpuParticleSpawnPipeline::Advance */
    Result<CpuParticleSpawnStepResult> CpuParticleSpawnPipeline::Advance(const CpuParticleSpawnStep &step) {
        if (state_ == nullptr || state_->shutDown)
            return Failure<CpuParticleSpawnStepResult>(VfxErrors::ParticleBufferShutDown);
        if (step.cancelled)
            return Failure<CpuParticleSpawnStepResult>(VfxErrors::ParticleSpawnStepCancelled);
        if (!std::isfinite(step.deltaSeconds) || step.deltaSeconds < 0.0F || step.deltaSeconds > state_->info.maximumDeltaSeconds ||
            step.burstCount > state_->info.maximumBurstParticles)
            return Failure<CpuParticleSpawnStepResult>(VfxErrors::ParticleSpawnStepInvalid);
        if (auto owner = state_->buffer.View(); owner.HasError())
            return Result<CpuParticleSpawnStepResult>::Failure(owner.ErrorValue());

        auto plan = CalculateBirthPlan(*state_, step);
        if (plan.HasError())
            return Result<CpuParticleSpawnStepResult>::Failure(plan.ErrorValue());
        auto firstDense = SpawnParticles(*state_, plan.Value().admitted);
        if (firstDense.HasError())
            return Result<CpuParticleSpawnStepResult>::Failure(firstDense.ErrorValue());
        auto view = state_->buffer.View();
        if (view.HasError())
            return Result<CpuParticleSpawnStepResult>::Failure(view.ErrorValue());

        state_->spawnCarry = plan.Value().carry;
        CpuParticleSoAView mutableView = view.Value();
        InitializeParticles(*state_, mutableView, firstDense.Value(), plan.Value().admitted);
        AdvanceAges(mutableView, step.deltaSeconds);
        auto reclaimed = ReclaimParticles(*state_, mutableView);
        if (reclaimed.HasError())
            return Result<CpuParticleSpawnStepResult>::Failure(reclaimed.ErrorValue());

        const std::uint32_t active = state_->buffer.Statistics().active;
        const std::uint64_t dropped = plan.Value().requested - plan.Value().admitted;
        ++state_->statistics.steps;
        state_->statistics.spawned += plan.Value().admitted;
        state_->statistics.dropped += dropped;
        state_->statistics.killed += reclaimed.Value();
        state_->statistics.nextSpawnOrdinal += plan.Value().admitted;
        return Result<CpuParticleSpawnStepResult>::Success({.requestedBirths = plan.Value().requested,
                                                            .spawned = plan.Value().admitted,
                                                            .dropped = dropped,
                                                            .killed = reclaimed.Value(),
                                                            .active = active});
    }

    /** @copydoc CpuParticleSpawnPipeline::SignalKill */
    Result<void> CpuParticleSpawnPipeline::SignalKill(const CpuParticleHandle &handle) {
        if (state_ == nullptr || state_->shutDown)
            return Failure<void>(VfxErrors::ParticleBufferShutDown);
        auto dense = state_->buffer.ResolveDenseIndex(handle);
        if (dense.HasError())
            return Result<void>::Failure(dense.ErrorValue());
        auto view = state_->buffer.View();
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        view.Value().customFlags[dense.Value()] |= ExplicitKillBit;
        return Result<void>::Success();
    }

    /** @copydoc CpuParticleSpawnPipeline::HandleAtDenseIndex */
    Result<CpuParticleHandle> CpuParticleSpawnPipeline::HandleAtDenseIndex(const std::uint32_t dense) {
        if (state_ == nullptr || state_->shutDown)
            return Failure<CpuParticleHandle>(VfxErrors::ParticleBufferShutDown);
        if (auto owner = state_->buffer.View(); owner.HasError())
            return Result<CpuParticleHandle>::Failure(owner.ErrorValue());
        if (dense >= state_->buffer.Statistics().active)
            return Failure<CpuParticleHandle>(VfxErrors::ParticleHandleInvalid);
        return Result<CpuParticleHandle>::Success(state_->handles[dense]);
    }

    /** @copydoc CpuParticleSpawnPipeline::View */
    Result<CpuParticleSoAView> CpuParticleSpawnPipeline::View() {
        if (state_ == nullptr || state_->shutDown)
            return Failure<CpuParticleSoAView>(VfxErrors::ParticleBufferShutDown);
        return state_->buffer.View();
    }

    /** @copydoc CpuParticleSpawnPipeline::Statistics */
    CpuParticleSpawnStatistics CpuParticleSpawnPipeline::Statistics() const noexcept {
        return state_ == nullptr ? CpuParticleSpawnStatistics{} : state_->statistics;
    }

    /** @copydoc CpuParticleSpawnPipeline::Shutdown */
    Result<void> CpuParticleSpawnPipeline::Shutdown() {
        if (state_ == nullptr || state_->shutDown)
            return Result<void>::Success();
        if (auto result = state_->buffer.Shutdown(); result.HasError())
            return result;
        state_->shutDown = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Vfx
