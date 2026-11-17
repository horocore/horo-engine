#include "CpuParticleSimulatorInternal.h"

namespace Horo::Vfx {
    namespace {
        /** @brief Preserves all monotonic identity counters while retiring the prior activation's transient inputs. */
        void ClearActivation(Detail::CpuParticleSimulatorState &state, const EffectSystemId activation) noexcept {
            static_cast<void>(state.committed.Clear());
            static_cast<void>(state.candidate.Clear());
            state.killRequests.clear();
            state.spawnCarry = 0.0;
            state.activation = activation;
            for (std::uint32_t index = 0; index < state.payloadChannelCount; ++index)
                state.inputValues[index] = state.payloadChannels[index].minimum;
            state.spawnRate =
                CpuParticleSimulatorDetail::SampleRange(state, ParticleSimulationId::Create(state.nextSimulationIdentity).Value(), 4,
                                                        state.descriptor.spawnRate);
        }

        /** @brief Computes vector storage using actual capacity rather than requested element counts. */
        template <typename Value> [[nodiscard]] std::uint64_t Bytes(const std::vector<Value> &values) noexcept {
            return static_cast<std::uint64_t>(values.capacity()) * sizeof(Value);
        }
    }  // namespace

    /** @copydoc CpuParticleSimulator::Reset */
    CpuParticleResetStatus CpuParticleSimulator::Reset(const EffectSystemId activation) noexcept {
        using enum CpuParticleResetStatus;
        if (!state_ || state_->shutDown)
            return ShutDown;
        if (state_->ownerThread != std::this_thread::get_id())
            return ThreadViolation;
        if (state_->advancing)
            return ActiveStep;
        if (!activation.IsValid() || activation.scope != state_->activation.scope || activation.slot != state_->activation.slot ||
            activation.generation <= state_->activation.generation)
            return InvalidActivation;
        if (state_->committedGeneration == std::numeric_limits<std::uint64_t>::max())
            return GenerationExhausted;
        ClearActivation(*state_, activation);
        ++state_->committedGeneration;
        return Complete;
    }

    /** @copydoc CpuParticleSimulator::PreparedBytes */
    std::uint64_t CpuParticleSimulator::PreparedBytes() const noexcept {
        if (!state_)
            return 0;
        const auto &state = *state_;
        return sizeof(state) + state.committed.Statistics().allocatedBytes + state.candidate.Statistics().allocatedBytes +
               Bytes(state.depthSamples) + Bytes(state.handles) + Bytes(state.killRequests) + Bytes(state.survivorSources) +
               Bytes(state.previousPositions) + Bytes(state.acceleration) + Bytes(state.baseSizeX) + Bytes(state.baseSizeY) +
               Bytes(state.baseColor);
    }
}  // namespace Horo::Vfx
