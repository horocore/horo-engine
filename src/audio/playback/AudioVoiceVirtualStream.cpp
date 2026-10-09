#include "AudioVoiceRenderState.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Audio {
    /** @copydoc AudioVoiceRenderRuntime::State::StreamControl */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::StreamControl(const AudioVoiceControlRequest &request) noexcept {
        using enum AudioVoiceControl;
        using enum AudioVoiceState;
        if (!seekable)
            return &AudioErrors::OperationUnsupported;
        switch (request.control) {
            case StartVirtual:
                return EnterVirtualStream(Ready);
            case Virtualize:
                return EnterVirtualStream(Playing);
            case Realize:
                return RealizeStream();
            case Seek:
            case SetLoop:
                return PositionStream(request);
            default:
                return &AudioErrors::OperationUnsupported;
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::State::EnterVirtualStream */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::EnterVirtualStream(const AudioVoiceState expected) noexcept {
        using enum AudioVoiceState;
        if (callback.streamState != expected)
            return &AudioErrors::VoiceInvalidTransition;
        if (expected == Ready) {
            if (const auto *error = registry->TryTransition(voice, Scheduled))
                return error;
        }
        if (const auto *error = registry->TryTransition(voice, Virtual))
            return error;
        streamPlayback.virtualMode = true;
        callback.streamState = Virtual;
        stream->SuspendFills(true);
        return nullptr;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RealizeStream */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::RealizeStream() noexcept {
        if (callback.streamState != AudioVoiceState::Virtual || streamPlayback.realizing)
            return &AudioErrors::VoiceInvalidTransition;
        if (!stream->RequestPosition(streamPlayback.cursor.frame, streamPlayback.loop))
            return &AudioErrors::VoiceAdmissionClosed;
        stream->SuspendFills(false);
        streamPlayback.realizing = true;
        streamPlayback.catchupFrames = 0;
        callback.buffered = 0;
        callback.sourceEnded = false;
        spatial->Reset();
        return nullptr;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::PositionStream */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::PositionStream(const AudioVoiceControlRequest &request) noexcept {
        using enum AudioVoiceControl;
        if (!CanPositionStream())
            return &AudioErrors::VoiceInvalidTransition;
        if (!ValidStreamPosition(request))
            return &AudioErrors::PlaybackRequestInvalid;
        auto next = streamPlayback.cursor;
        auto loop = streamPlayback.loop;
        if (request.control == Seek) {
            next = {.frame = request.seekFrame};
        } else {
            loop = request.loop;
            if (loop.enabled && next.frame >= loop.end)
                next = {.frame = loop.begin};
        }
        if (!streamPlayback.virtualMode && !stream->RequestPosition(next.frame, loop))
            return &AudioErrors::VoiceAdmissionClosed;
        streamPlayback.cursor = next;
        streamPlayback.loop = loop;
        callback.buffered = 0;
        callback.sourceEnded = false;
        spatial->Reset();
        return nullptr;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::CanPositionStream */
    bool AudioVoiceRenderRuntime::State::CanPositionStream() const noexcept {
        using enum AudioVoiceState;
        const auto current = callback.streamState;
        return !streamPlayback.realizing && (current == Ready || current == Playing || current == Virtual || current == Paused);
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ValidStreamPosition */
    bool AudioVoiceRenderRuntime::State::ValidStreamPosition(const AudioVoiceControlRequest &request) const noexcept {
        if (request.control == AudioVoiceControl::Seek)
            return request.seekFrame <= sourceFrames && (!streamPlayback.loop.enabled || request.seekFrame < streamPlayback.loop.end);
        const auto loop = request.loop;
        return loop.enabled ? loop.begin < loop.end && loop.end <= sourceFrames : loop.begin == 0 && loop.end == 0;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::AdvanceStream */
    void AudioVoiceRenderRuntime::State::AdvanceStream(const double sourceAdvance) noexcept {
        auto &position = streamPlayback.cursor;
        const double advanced = position.fraction + sourceAdvance;
        const auto whole = static_cast<std::uint64_t>(advanced);
        position.fraction = advanced - static_cast<double>(whole);
        const auto frame = position.frame + whole;  // Admitted stream <=2^40, one call <=4096*64.
        const auto loop = streamPlayback.loop;
        if (loop.enabled && frame >= loop.end)
            position.frame = loop.begin + (frame - loop.begin) % (loop.end - loop.begin);
        else if (frame >= sourceFrames)
            position = {.frame = sourceFrames};
        else
            position.frame = frame;
        if (streamPlayback.realizing) {
            const auto prior = streamPlayback.catchupFrames;
            streamPlayback.catchupFrames =
                whole > std::numeric_limits<std::uint64_t>::max() - prior ? std::numeric_limits<std::uint64_t>::max() : prior + whole;
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::State::FinishVirtualStream */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::FinishVirtualStream() noexcept {
        (void)registry->TryTransition(voice, AudioVoiceState::Finished);
        stream->SuspendFills(true);
        streamPlayback.terminalReported = true;
        return {.terminal = true};
    }

    /** @copydoc AudioVoiceRenderRuntime::State::TryRealizeStream */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::TryRealizeStream(const std::uint32_t frames, bool &rendered) noexcept {
        auto &logical = streamPlayback;
        if (logical.realizing && stream->PositionReady()) {
            // Catch up only admitted ring frames, not elapsed wall time or unbounded PCM. If worker
            // preparation fell farther behind, reseek the current logical phase and retain Virtual.
            if (logical.catchupFrames > MaximumAudioCallbackFrames * 64ULL) {
                if (stream->RequestPosition(logical.cursor.frame, logical.loop))
                    logical.catchupFrames = 0;
            } else {
                logical.catchupFrames -= stream->Discard(static_cast<std::uint32_t>(logical.catchupFrames));
                if (logical.catchupFrames == 0 && stream->AvailableFrames() != 0) {
                    rendered = true;
                    if (const auto *error = registry->TryTransition(voice, AudioVoiceState::Playing))
                        return {.error = error};
                    callback.streamState = AudioVoiceState::Playing;
                    logical.virtualMode = false;
                    logical.realizing = false;
                    return RenderStream(frames);
                }
            }
        }
        return {};
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RenderVirtualStream */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderVirtualStream(const std::uint32_t frames) noexcept {
        auto &logical = streamPlayback;
        if (stream->IsStopped()) {
            (void)registry->TryCancel(voice);
            logical.terminalReported = true;
            return {.terminal = true};
        }
        if (!logical.loop.enabled && logical.cursor.frame == sourceFrames)
            return FinishVirtualStream();
        bool rendered{};
        const auto realized = TryRealizeStream(frames, rendered);
        if (rendered)
            return realized;
        const double step = static_cast<double>(conversion.inputRate) / conversion.outputRate * callback.active->target.pitch;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            AdvanceStream(step);
            if (!logical.loop.enabled && logical.cursor.frame == sourceFrames)
                return FinishVirtualStream();
        }
        return {};
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RenderStream */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderStream(const std::uint32_t frames) noexcept {
        if (const auto *error = registry->CheckState(voice, callback.streamState))
            return {.error = error};
        if (IsTerminalAudioVoiceState(callback.streamState)) {
            const bool first = !streamPlayback.terminalReported;
            streamPlayback.terminalReported = true;
            return {.terminal = first};
        }
        if (callback.streamState == AudioVoiceState::Virtual)
            return RenderVirtualStream(frames);
        if (callback.streamState != AudioVoiceState::Playing)
            return {};
        return RenderStreamPcm(frames);
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RenderStreamPcm */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderStreamPcm(const std::uint32_t frames) noexcept {
        std::array<AudioSample *, 2> rawPointers{scratch.raw[0].samples.data(), scratch.raw[1].samples.data()};
        std::uint32_t produced{};
        // Linear conversion admits at most 64 input frames per output frame. Each full chunk covers
        // maximumFrames >= frames; 64 chunks plus two history/end-marker visits bound even high-ratio
        // conversion without the short-block truncation of a frames+1 visit budget.
        std::uint32_t visit{};
        while (visit < 66 && produced < frames) {
            ++visit;
            if (ReadStreamBlock({rawPointers.data(), conversion.channels}))
                return {.terminal = true};
            const auto progress = ConvertStreamBlock(frames, produced);
            if (progress.status == AudioResamplerStatus::InvalidBuffer || progress.status == AudioResamplerStatus::InvalidState)
                return {.error = &AudioErrors::ResamplerInvalid};
            produced += progress.produced;
            if (progress.status == AudioResamplerStatus::Complete) {
                (void)registry->TryTransition(voice, AudioVoiceState::Finished);
                callback.streamState = AudioVoiceState::Finished;
                streamPlayback.terminalReported = true;
                return {.terminal = true};
            }
            if (progress.produced == 0 && progress.consumed == 0)
                break;  // Silence is already prepared; starvation never waits for a fill job.
        }
        return {};
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ReadStreamBlock */
    bool AudioVoiceRenderRuntime::State::ReadStreamBlock(const std::span<AudioSample *const> rawPointers) noexcept {
        if (callback.buffered != 0 || callback.sourceEnded)
            return false;
        const auto read = stream->Render(rawPointers, descriptor.maximumFrames);
        callback.buffered = read.availableFrames;
        callback.sourceEnded = read.ended || read.stopped;
        if (!read.stopped)
            return false;
        (void)registry->TryCancel(voice);
        callback.streamState = AudioVoiceState::Cancelled;
        streamPlayback.terminalReported = true;
        return true;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ConvertStreamBlock */
    AudioResamplerProgress AudioVoiceRenderRuntime::State::ConvertStreamBlock(const std::uint32_t frames,
                                                                              const std::uint32_t produced) noexcept {
        std::array<std::span<const float>, 2> inputs{std::span<const float>{scratch.raw[0].samples}.first(callback.buffered),
                                                     std::span<const float>{scratch.raw[1].samples}.first(callback.buffered)};
        std::array<std::span<float>, 2> outputs{std::span{scratch.converted[0].samples}.first(frames - produced),
                                                std::span{scratch.converted[1].samples}.first(frames - produced)};
        const auto progress =
            spatial->Process({{inputs.data(), conversion.channels}, callback.buffered, callback.sourceEnded}, {outputs, frames - produced});
        if (progress.status == AudioResamplerStatus::InvalidBuffer || progress.status == AudioResamplerStatus::InvalidState)
            return progress;
        for (std::uint32_t channel = 0; channel < 2; ++channel)
            std::copy_n(scratch.converted[channel].samples.begin(), progress.produced, scratch.output[channel].samples.begin() + produced);
        AdvanceStream(progress.sourceAdvance);
        callback.buffered -= progress.consumed;
        for (std::uint32_t channel = 0; channel < conversion.channels; ++channel)
            std::move(scratch.raw[channel].samples.begin() + progress.consumed,
                      scratch.raw[channel].samples.begin() + progress.consumed + callback.buffered, scratch.raw[channel].samples.begin());
        return progress;
    }

}  // namespace Horo::Audio
