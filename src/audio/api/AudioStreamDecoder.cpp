#include "Horo/Audio/AudioStreamDecoder.h"

#include "Horo/Audio/AudioStreamDecoderErrors.h"

#include <utility>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] bool ValidLimits(const AudioStreamDecoderLimits &limits) noexcept {
            return limits.maximumFrames > 0 && limits.maximumFramesPerDecode > 0 &&
                   limits.maximumFramesPerDecode <= MaximumAudioCallbackFrames && limits.maximumChannels > 0 &&
                   limits.maximumChannels <= MaximumAudioChannels && limits.maximumWorkingBytes > 0;
        }

        [[nodiscard]] Result<void> Validate(const AudioStreamDecoderSpec &spec, const AudioStreamDecoderProvider &provider,
                                            const AudioStreamDecoderLimits &limits) {
            if (!ValidLimits(limits) || !spec.codec.IsValid() ||
                !ValidateAudioProcessingFormat(spec.outputFormat, limits.maximumChannels) || spec.maximumFramesPerDecode == 0 ||
                provider.context == nullptr || provider.decode == nullptr || provider.release == nullptr ||
                (provider.seek != nullptr) != spec.seekable)
                return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
            if (spec.frameCount > limits.maximumFrames || spec.maximumFramesPerDecode > limits.maximumFramesPerDecode ||
                spec.requiredWorkingBytes > limits.maximumWorkingBytes)
                return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::CapacityExceeded));
            return Result<void>::Success();
        }
    }  // namespace

    AudioStreamDecoder::AudioStreamDecoder(AudioStreamDecoderSpec spec, const AudioStreamDecoderProvider &provider) noexcept
        : spec_(std::move(spec)), provider_(provider),
          state_(spec_.frameCount == 0 ? AudioStreamDecoderState::Ended : AudioStreamDecoderState::Ready) {}

    /** @copydoc AudioStreamDecoder::Create */
    Result<AudioStreamDecoder> AudioStreamDecoder::Create(AudioStreamDecoderSpec spec, const AudioStreamDecoderProvider &provider,
                                                          const AudioStreamDecoderLimits &limits) {
        if (const auto valid = Validate(spec, provider, limits); valid.HasError())
            return Result<AudioStreamDecoder>::Failure(valid.ErrorValue());
        return Result<AudioStreamDecoder>::Success(AudioStreamDecoder{std::move(spec), provider});
    }

    /** @copydoc AudioStreamDecoder::AudioStreamDecoder */
    AudioStreamDecoder::AudioStreamDecoder(AudioStreamDecoder &&other) noexcept
        : spec_(std::move(other.spec_)), provider_(other.provider_), cancelled_(other.cancelled_.load()), cursor_(other.cursor_),
          state_(other.state_) {
        other.provider_ = {};
        other.state_ = AudioStreamDecoderState::Closed;
    }

    /** @copydoc AudioStreamDecoder::~AudioStreamDecoder */
    AudioStreamDecoder::~AudioStreamDecoder() {
        Close();
    }

    /** @copydoc AudioStreamDecoder::Decode */
    Result<AudioStreamDecodedBlock> AudioStreamDecoder::Decode(const std::span<AudioSample> output,
                                                               const std::span<std::byte> workingMemory,
                                                               const std::uint32_t requestedFrames) {
        if (state_ == AudioStreamDecoderState::Closed || state_ == AudioStreamDecoderState::Failed ||
            state_ == AudioStreamDecoderState::Cancelled)
            return Result<AudioStreamDecodedBlock>::Failure(MakeError(AudioStreamDecoderErrors::LifecycleUnavailable));
        if (cancelled_.load()) {
            state_ = AudioStreamDecoderState::Cancelled;
            return Result<AudioStreamDecodedBlock>::Failure(MakeError(AudioStreamDecoderErrors::Cancelled));
        }
        if (requestedFrames == 0)
            return Result<AudioStreamDecodedBlock>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
        if (requestedFrames > spec_.maximumFramesPerDecode || workingMemory.size() < spec_.requiredWorkingBytes ||
            output.size() < static_cast<std::size_t>(requestedFrames) * spec_.outputFormat.layout.orderedChannels.size())
            return Result<AudioStreamDecodedBlock>::Failure(MakeError(AudioStreamDecoderErrors::CapacityExceeded));
        if (state_ == AudioStreamDecoderState::Ended)
            return Result<AudioStreamDecodedBlock>::Success({cursor_, 0, true});

        const std::uint64_t firstFrame = cursor_;
        auto produced = [&]() {
            // Provider exceptions, including non-std ones, must not escape this worker boundary.
            try {
                return provider_.decode(provider_.context, firstFrame,
                                        output.first(static_cast<std::size_t>(requestedFrames) *
                                                     spec_.outputFormat.layout.orderedChannels.size()),
                                        workingMemory.first(spec_.requiredWorkingBytes), cancelled_);
            } catch (...) {  // NOSONAR
                return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioStreamDecoderErrors::ProviderFailed));
            }
        }();
        if (cancelled_.load()) {
            state_ = AudioStreamDecoderState::Cancelled;
            return Result<AudioStreamDecodedBlock>::Failure(MakeError(AudioStreamDecoderErrors::Cancelled));
        }
        if (produced.HasError()) {
            state_ = AudioStreamDecoderState::Failed;
            return Result<AudioStreamDecodedBlock>::Failure(std::move(produced).ErrorValue());
        }

        const AudioStreamDecodeProgress progress = produced.Value();
        if (progress.frames > requestedFrames || progress.frames > spec_.frameCount - cursor_ ||
            (progress.frames == 0 && !progress.endOfStream) || progress.endOfStream != (cursor_ + progress.frames == spec_.frameCount)) {
            state_ = AudioStreamDecoderState::Failed;
            return Result<AudioStreamDecodedBlock>::Failure(MakeError(AudioStreamDecoderErrors::ProtocolViolation));
        }
        cursor_ += progress.frames;
        if (progress.endOfStream)
            state_ = AudioStreamDecoderState::Ended;
        return Result<AudioStreamDecodedBlock>::Success({firstFrame, progress.frames, progress.endOfStream});
    }

    /** @copydoc AudioStreamDecoder::Seek */
    Result<void> AudioStreamDecoder::Seek(const std::uint64_t targetFrame) {
        if (state_ == AudioStreamDecoderState::Closed || state_ == AudioStreamDecoderState::Failed ||
            state_ == AudioStreamDecoderState::Cancelled)
            return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::LifecycleUnavailable));
        if (cancelled_.load()) {
            state_ = AudioStreamDecoderState::Cancelled;
            return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::Cancelled));
        }
        if (!spec_.seekable)
            return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::SeekUnsupported));
        if (targetFrame > spec_.frameCount)
            return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
        // Provider exceptions, including non-std ones, must not escape this worker boundary.
        try {
            auto sought = provider_.seek(provider_.context, targetFrame, cancelled_);
            if (cancelled_.load()) {
                state_ = AudioStreamDecoderState::Cancelled;
                return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::Cancelled));
            }
            if (sought.HasError()) {
                state_ = AudioStreamDecoderState::Failed;
                return sought;
            }
        } catch (...) {  // NOSONAR
            state_ = AudioStreamDecoderState::Failed;
            return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::ProviderFailed));
        }
        cursor_ = targetFrame;
        state_ = cursor_ == spec_.frameCount ? AudioStreamDecoderState::Ended : AudioStreamDecoderState::Ready;
        return Result<void>::Success();
    }

    /** @copydoc AudioStreamDecoder::Cancel */
    void AudioStreamDecoder::Cancel() noexcept {
        cancelled_.store(true);
    }

    /** @copydoc AudioStreamDecoder::Close */
    void AudioStreamDecoder::Close() noexcept {
        if (provider_.release != nullptr) {
            provider_.release(provider_.context);
            provider_ = {};
        }
        state_ = AudioStreamDecoderState::Closed;
    }

    /** @copydoc AudioStreamDecoder::State */
    AudioStreamDecoderState AudioStreamDecoder::State() const noexcept {
        return state_;
    }

    /** @copydoc AudioStreamDecoder::CursorFrame */
    std::uint64_t AudioStreamDecoder::CursorFrame() const noexcept {
        return cursor_;
    }

    /** @copydoc AudioStreamDecoder::Spec */
    const AudioStreamDecoderSpec &AudioStreamDecoder::Spec() const noexcept {
        return spec_;
    }
}  // namespace Horo::Audio
