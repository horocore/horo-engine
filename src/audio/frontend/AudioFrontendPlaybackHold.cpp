#include "AudioFrontendState.h"

#include <limits>

namespace Horo::Audio {
    namespace {
        /** @brief Fade one complete block with exact positive-zero endpoints and no persistent scratch. */
        void FadeBlock(const AudioPlanarBlockView &block, const bool incoming) noexcept {
            const auto frames = block.validFrames;
            for (auto *plane : block.planes)
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    const float ramp = frames <= 1 ? 0.0F : static_cast<float>(frame) / static_cast<float>(frames - 1);
                    const float gain = frames <= 1 ? 0.0F : incoming ? ramp : 1.0F - ramp;
                    plane[frame] = gain == 0.0F ? 0.0F : plane[frame] * gain;
                }
        }
    }  // namespace

    /** @copydoc AudioFrontend::State::RenderPlayback */
    Backend::RenderResult AudioFrontend::State::RenderPlayback(const Backend::RenderInvocation &invocation) noexcept {
        const auto requested = holdRequested.load(std::memory_order_acquire);
        bool resuming{};
        if (processingHeld) {
            if (requested != 0) {
                if (holdAcknowledged.load(std::memory_order_relaxed) != requested) {
                    // A new hold admitted before any resumed callback preserves the same audible boundary.
                    heldPlayback.operation.sequence = requested;
                    holdAcknowledged.store(requested, std::memory_order_release);
                }
                return {Backend::RenderDisposition::Rendered, AudioCallbackFaultCode::None};
            }
            processingHeld = false;
            resuming = true;
        }
        if (requested != 0 && invocation.sampleFrame > std::numeric_limits<std::uint64_t>::max() - invocation.output.validFrames)
            return {Backend::RenderDisposition::Fault, AudioCallbackFaultCode::InvalidEpoch};
        const auto rendered = RenderBlock(invocation);
        if (rendered.disposition != Backend::RenderDisposition::Rendered)
            return rendered;
        if (requested != 0) {
            FadeBlock(invocation.output, false);
            heldPlayback = {{resources->scope.owner, requested},
                            output->open.plannedEpoch,
                            resources->voice->Cursor(),
                            invocation.sampleFrame + invocation.output.validFrames,
                            invocation.startedAt};
            processingHeld = true;
            holdAcknowledged.store(requested, std::memory_order_release);
        } else if (resuming)
            FadeBlock(invocation.output, true);
        return rendered;
    }

    /** @copydoc Internal::AudioFrontendComposition::RequestPlaybackHold */
    Result<Internal::AudioFrontendPlaybackHoldId> Internal::AudioFrontendComposition::RequestPlaybackHold(AudioFrontend &frontend) {
        using Outcome = Result<AudioFrontendPlaybackHoldId>;
        auto &state = *frontend.state_;
        if (state.phase != AudioFrontendPhase::Active || state.closing || state.pending || state.holdRequested.load() != 0)
            return Outcome::Failure(MakeError(AudioErrors::RuntimeInactive));
        for (const auto &receipt : state.receipts)
            if (receipt.sequence.load(std::memory_order_acquire) != 0 &&
                receipt.status.load(std::memory_order_acquire) == AudioFrontend::State::OperationStatus::Pending)
                return Outcome::Failure(MakeError(AudioErrors::QueueSaturated));
        if (state.holdSequence == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
        const AudioFrontendPlaybackHoldId operation{state.resources->scope.owner, ++state.holdSequence};
        state.holdRequested.store(operation.sequence, std::memory_order_release);
        return Outcome::Success(operation);
    }

    /** @copydoc Internal::AudioFrontendComposition::PollPlaybackHold */
    Result<std::optional<Internal::AudioFrontendHeldPlayback>> Internal::AudioFrontendComposition::PollPlaybackHold(
        const AudioFrontend &frontend, const AudioFrontendPlaybackHoldId operation) {
        using Outcome = Result<std::optional<AudioFrontendHeldPlayback>>;
        const auto &state = *frontend.state_;
        if (operation.owner != state.resources->scope.owner)
            return Outcome::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
        if (operation.sequence == 0 || operation.sequence != state.holdSequence)
            return Outcome::Failure(MakeError(AudioErrors::HandleStale));
        if (state.phase != AudioFrontendPhase::Active || state.closing || state.holdRequested.load() != operation.sequence)
            return Outcome::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (state.holdAcknowledged.load(std::memory_order_acquire) != operation.sequence)
            return Outcome::Success(std::nullopt);
        return Outcome::Success(state.heldPlayback);
    }

    /** @copydoc Internal::AudioFrontendComposition::ReleasePlaybackHold */
    Result<void> Internal::AudioFrontendComposition::ReleasePlaybackHold(AudioFrontend &frontend,
                                                                         const AudioFrontendPlaybackHoldId operation) {
        const auto held = PollPlaybackHold(frontend, operation);
        if (held.HasError())
            return Result<void>::Failure(held.ErrorValue());
        if (!held.Value())
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        frontend.state_->holdRequested.store(0, std::memory_order_release);
        return Result<void>::Success();
    }
}  // namespace Horo::Audio
