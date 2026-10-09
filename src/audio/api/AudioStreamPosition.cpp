#include "AudioStreamState.h"

#include <algorithm>

namespace Horo::Audio {
    namespace {
        /** @brief Validate exclusive loop and initial cursor against admitted immutable source facts. */
        bool ValidPosition(const AudioStreamState &state, const std::uint64_t frame, const AudioVoiceLoop loop) noexcept {
            if (frame > state.request.decoder.frameCount)
                return false;
            return loop.enabled ? loop.begin < loop.end && loop.end <= state.request.decoder.frameCount && frame < loop.end
                                : loop.begin == 0 && loop.end == 0;
        }

        /** @brief Sole-consumer acquired ring count, bounded even across a worker publication reset. */
        std::uint32_t Available(const AudioStreamState *state) noexcept {
            if (!state || !state->transport.positionReady.load() || state->stopped.load())
                return 0;
            const auto read = state->transport.consumed.load();
            const auto written = state->transport.produced.load() & CursorMask;
            return written >= read ? static_cast<std::uint32_t>(std::min<std::uint64_t>(written - read, state->request.ringFrames)) : 0;
        }
    }  // namespace

    /** @copydoc AudioStreamRenderPort::RequestPosition */
    bool AudioStreamRenderPort::RequestPosition(const std::uint64_t frame, const AudioVoiceLoop loop) const noexcept {
        if (!state_ || !state_->request.decoder.seekable || state_->stopped.load() || !state_->transport.positionReady.load())
            return false;
        if (!ValidPosition(*state_, frame, loop))
            return false;
        state_->transport.positionReady.store(false);
        // No ring reads after this point. An old bounded fill may finish; its samples remain unread until worker reset/ack.
        state_->transport.consumed.store(0);
        state_->transport.requestedLoop.store(loop.enabled ? (static_cast<std::uint64_t>(loop.begin) << 32U) | loop.end : 0);
        state_->transport.seekTarget.store(frame);
        return true;
    }

    /** @copydoc AudioStreamRenderPort::PositionReady */
    bool AudioStreamRenderPort::PositionReady() const noexcept {
        return state_ && state_->transport.positionReady.load();
    }

    /** @copydoc AudioStreamRenderPort::SuspendFills */
    void AudioStreamRenderPort::SuspendFills(const bool suspended) const noexcept {
        if (state_)
            state_->transport.fillsSuspended.store(suspended);
    }

    /** @copydoc AudioStreamRenderPort::IsStopped */
    bool AudioStreamRenderPort::IsStopped() const noexcept {
        return !state_ || state_->stopped.load();
    }

    /** @copydoc AudioStreamRenderPort::Discard */
    std::uint32_t AudioStreamRenderPort::Discard(const std::uint32_t frames) const noexcept {
        const auto available = std::min(frames, Available(state_));
        if (available != 0)
            state_->transport.consumed.store(state_->transport.consumed.load() + available);
        return available;
    }

    /** @copydoc AudioStreamRenderPort::AvailableFrames */
    std::uint32_t AudioStreamRenderPort::AvailableFrames() const noexcept {
        return Available(state_);
    }
}  // namespace Horo::Audio
