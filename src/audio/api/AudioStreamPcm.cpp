#include "AudioStreamState.h"
#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace Horo::Audio {
    /** @copydoc AudioStreamState::ValidSamples */
    bool AudioStreamState::ValidSamples(const std::span<const AudioSample> samples) noexcept {
        return std::ranges::all_of(samples, [](const AudioSample sample) { return std::isfinite(sample); });
    }

    /** @copydoc AudioStreamState::PublishBlock */
    void AudioStreamState::PublishBlock(const std::uint64_t written, const AudioStreamDecodedBlock progress,
                                        const std::span<const AudioSample> samples) noexcept {
        const auto channels = request.decoder.outputFormat.layout.orderedChannels.size();
        for (std::uint32_t frame = 0; frame < progress.frames; ++frame) {
            const auto destination = static_cast<std::size_t>((written + frame) % request.ringFrames) * channels;
            const auto input = samples.subspan(static_cast<std::size_t>(frame) * channels, channels);
            std::ranges::transform(input, ring.data() + destination, [](const AudioSample sample) {
                return sample == 0.0F || std::fpclassify(sample) == FP_SUBNORMAL ? 0.0F : sample;
            });
        }
        transport.produced.store((written + progress.frames) | (progress.endOfStream ? EndMarker : 0));
    }

    /** @copydoc AudioStreamState::ScrubLivePcm */
    void AudioStreamState::ScrubLivePcm() noexcept {
        volatile AudioSample *const output = ring.data();
        for (std::size_t index = 0; index < ring.size(); ++index)
            output[index] = 0.0F;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    /** @copydoc AudioStreamingService::CreateLive */
    Result<std::unique_ptr<AudioStreamingService>> AudioStreamingService::CreateLive(JobSystem &jobs, const AudioStreamingLimits limits) {
        return CreateService(jobs, {}, limits);
    }

    /** @copydoc AudioStreamingService::SubmitLivePcm */
    Result<void> AudioStreamingService::SubmitLivePcm(const AudioStreamHandle handle, const std::span<const AudioSample> samples) {
        auto *state = Find(handle);
        if (!state)
            return Result<void>::Failure(MakeError(AudioErrors::HandleStale));
        if (!state->IsLivePcm() || state->stopped.load())
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        const auto channels = state->request.decoder.outputFormat.layout.orderedChannels.size();
        const auto frames = samples.size() / channels;
        if (frames == 0 || frames > MaximumAudioCallbackFrames || samples.size() != frames * channels)
            return Result<void>::Failure(MakeError(AudioErrors::StreamReadFailed));
        const auto written = state->transport.produced.load();
        const auto buffered = written - state->transport.consumed.load();
        if (buffered + frames > state->request.ringFrames || frames > CursorMask - written)
            return Result<void>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        if (!AudioStreamState::ValidSamples(samples))
            return Result<void>::Failure(MakeError(AudioErrors::StreamReadFailed));
        state->PublishBlock(written, {.frames = static_cast<std::uint32_t>(frames)}, samples);
        return Result<void>::Success();
    }

    /** @copydoc AudioStreamingService::PlaybackFacts */
    Result<AudioStreamPlaybackFacts> AudioStreamingService::PlaybackFacts(const AudioStreamHandle handle) const {
        using Output = Result<AudioStreamPlaybackFacts>;
        const auto *state = Find(handle);
        if (!state)
            return Output::Failure(MakeError(AudioErrors::HandleStale));
        const auto &spec = state->request.decoder;
        try {
            const auto duration = state->IsLivePcm() ? std::nullopt : std::optional{spec.frameCount};
            return Output::Success({spec.outputFormat, duration, spec.seekable});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio
