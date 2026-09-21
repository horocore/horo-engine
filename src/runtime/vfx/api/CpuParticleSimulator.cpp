#include "Horo/Vfx/CpuParticleSimulator.h"

#include "CpuParticleSimulatorInternal.h"
#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Vfx::CpuParticleSimulatorDetail {
    Result<void> OwnerThreadResult(const Detail::CpuParticleSimulatorState &state) {
        if (std::this_thread::get_id() != state.ownerThread)
            return Failure<void>(VfxErrors::ParticleBufferThreadViolation);
        return Result<void>::Success();
    }

    Result<void> ValidateOperationalState(const Detail::CpuParticleSimulatorState *const state) {
        if (state == nullptr || state->shutDown)
            return Failure<void>(VfxErrors::ParticleBufferShutDown);
        return OwnerThreadResult(*state);
    }

    Result<void> Observe(const Detail::CpuParticleSimulatorState &state, const CpuParticleStage stage) {
        if (state.stageObserver == nullptr)
            return Result<void>::Success();
        return state.stageObserver(state.stageObserverContext, stage);
    }
}  // namespace Horo::Vfx::CpuParticleSimulatorDetail

namespace Horo::Vfx {
    namespace {
        using CpuParticleSimulatorDetail::Failure;
        using CpuParticleSimulatorDetail::Observe;
        using CpuParticleSimulatorDetail::OwnerThreadResult;
        using CpuParticleSimulatorDetail::PrepareCandidate;
        using CpuParticleSimulatorDetail::RunCollideStage;
        using CpuParticleSimulatorDetail::RunForcesStage;
        using CpuParticleSimulatorDetail::RunInitializeStage;
        using CpuParticleSimulatorDetail::RunIntegrateStage;
        using CpuParticleSimulatorDetail::RunKillStage;
        using CpuParticleSimulatorDetail::RunSpawnStage;
        using CpuParticleSimulatorDetail::ValidateOperationalState;
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
        auto prepared = CpuParticleSimulatorDetail::PrepareState(descriptor, info);
        if (prepared.HasError())
            return Result<CpuParticleSimulator>::Failure(prepared.ErrorValue());
        return Result<CpuParticleSimulator>::Success(CpuParticleSimulator{std::move(prepared).Value()});
    }

    /** @copydoc CpuParticleSimulator::Advance */
    Result<CpuParticleSimulationStepResult> CpuParticleSimulator::Advance(const CpuParticleSimulationStep &step) {
        if (const auto state = ValidateOperationalState(state_.get()); state.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(state.ErrorValue());
        if (step.cancelled)
            return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleSimulationStepCancelled);
        if (!CpuParticleSimulatorDetail::Finite(step.deltaSeconds) || step.deltaSeconds < 0.0F ||
            step.deltaSeconds > state_->maximumDeltaSeconds || step.burstCount > state_->maximumBurstParticles ||
            step.tick != state_->nextTick)
            return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleSpawnStepInvalid);
        if (state_->committedGeneration == std::numeric_limits<std::uint64_t>::max() ||
            state_->nextTick == std::numeric_limits<std::uint64_t>::max())
            return Failure<CpuParticleSimulationStepResult>(VfxErrors::ParticleGenerationStale);
        if (auto prepared = PrepareCandidate(*state_); prepared.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(prepared.ErrorValue());

        const auto spawn = RunSpawnStage(*state_, step);
        if (spawn.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(spawn.ErrorValue());
        if (auto initialized = RunInitializeStage(*state_, step, spawn.Value()); initialized.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(initialized.ErrorValue());
        if (auto forced = RunForcesStage(*state_, step); forced.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(forced.ErrorValue());
        if (auto integrated = RunIntegrateStage(*state_, step); integrated.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(integrated.ErrorValue());
        const auto collisions = RunCollideStage(*state_, step);
        if (collisions.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(collisions.ErrorValue());
        const auto killed = RunKillStage(*state_);
        if (killed.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(killed.ErrorValue());
        if (auto extracted = Observe(*state_, CpuParticleStage::Extract); extracted.HasError())
            return Result<CpuParticleSimulationStepResult>::Failure(extracted.ErrorValue());

        std::swap(state_->committed, state_->candidate);
        std::swap(state_->committedOffset, state_->candidateOffset);
        state_->spawnCarry = spawn.Value().plan.carry;
        state_->nextSimulationIdentity = spawn.Value().nextSimulationIdentity;
        ++state_->nextTick;
        ++state_->committedGeneration;
        ++state_->steps;
        state_->spawned += spawn.Value().plan.admitted;
        state_->dropped += spawn.Value().plan.requested - spawn.Value().plan.admitted;
        state_->killed += killed.Value();
        state_->collisions += collisions.Value();
        state_->killRequests.clear();
        return Result<CpuParticleSimulationStepResult>::Success({.requestedBirths = spawn.Value().plan.requested,
                                                                 .spawned = spawn.Value().plan.admitted,
                                                                 .dropped = spawn.Value().plan.requested - spawn.Value().plan.admitted,
                                                                 .killed = killed.Value(),
                                                                 .collisions = collisions.Value(),
                                                                 .active = state_->committed.Statistics().active,
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
            return Failure<CpuParticleHandle>(VfxErrors::ParticleHandleInvalid);
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
        const auto floats = [](const std::span<float> values) {
            return std::span<const float>{values.data(), values.size()};
        };
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
        if (!CpuParticleSimulatorDetail::Finite(value))
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
        const auto view = state_->committed.View();
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
