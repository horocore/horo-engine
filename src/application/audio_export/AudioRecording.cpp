#include "Horo/Application/Internal/AudioRecording.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioSourceImporter.h"
#include "Horo/Security/SecureMemory.h"
#include "WaveEncoding.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using namespace Audio;
        using namespace Audio::Backend;
        constexpr std::uint64_t MaximumRecordingBytes = 32U << 20U;
        using Published = Result<PublishedAudioRecording>;

        /** @brief Discards private bytes on every exit, including exceptional publication paths. */
        struct ScrubBytes final {
            explicit ScrubBytes(std::span<std::byte> storage) noexcept : bytes(storage) {}

            ScrubBytes(const ScrubBytes &) = delete;
            ScrubBytes &operator=(const ScrubBytes &) = delete;
            std::span<std::byte> bytes;

            ~ScrubBytes() {
                Security::SecureZero(bytes);
            }
        };

        /** @brief Reads the detached WAVE through the same bounded reader used by normal asset imports. */
        Result<std::size_t> ReadWave(std::span<const std::byte> wave, const std::uint64_t offset, std::span<std::byte> destination) {
            if (offset >= wave.size())
                return Result<std::size_t>::Success(0);
            const auto at = static_cast<std::size_t>(offset);
            const auto bytes = wave.subspan(at, std::min(destination.size(), wave.size() - at));
            std::ranges::copy(bytes, destination.begin());
            return Result<std::size_t>::Success(bytes.size());
        }

        /** @brief Validates complete decoding without retaining a second copy of the PCM. */
        Result<AudioSourceImportCandidate> ValidateWave(std::span<const std::byte> wave, const CancellationToken &cancellation) {
            auto stop = cancellation;
            const AudioSourceReader reader{&wave, wave.size(),
                                           [](auto *context, const std::uint64_t offset, std::span<std::byte> destination) {
                return ReadWave(*static_cast<const std::span<const std::byte> *>(context), offset, destination);
            }};
            const AudioDecodedFrameSink sink{&stop,
                                             [](auto *scope, std::uint64_t, const AudioProcessingFormat &, std::span<const AudioSample>) {
                return static_cast<const CancellationToken *>(scope)->IsCancellationRequested()
                           ? Result<void>::Failure(MakeError(AudioErrors::OperationCancelled))
                           : Result<void>::Success();
            }};
            return ImportCoreAudioSource(reader, sink);
        }

        /** @brief Scopes produced WAVE to its explicit stable identity and delegates filesystem transitions to AST. */
        Published CommitWave(std::vector<std::byte> wave, const AudioRecordingDestination &destination,
                             ProjectAssetImportCommitter &publisher, const CancellationToken &cancellation) {
            ScrubBytes initial{wave};
            auto validated = ValidateWave(wave, cancellation);
            if (validated.HasError())
                return Published::Failure(validated.ErrorValue());
            auto audioType = Assets::AssetTypeId::Parse("core.audio");
            if (audioType.HasError())
                return Published::Failure(audioType.ErrorValue());
            PreparedAuthoredProjectAssetBatch batch{.operationId = destination.operationId, .projectRoot = destination.projectRoot};
            batch.items.push_back({destination.id, std::move(audioType).Value(), destination.path, {}});
            auto &payload = batch.items.front().payload;
            payload = std::move(wave);
            // The payload must remain alive until the scrub after the synchronous publisher returns.
            ScrubBytes staged{payload};
            initial.bytes = {};
            if (const auto result = publisher.CommitAuthored(batch, cancellation); result.HasError())
                return Published::Failure(result.ErrorValue());
            const auto &candidate = validated.Value();
            return Published::Success({candidate.frameCount, candidate.durationNanoseconds, candidate.decodedFormat.sampleRate});
        }
    }  // namespace

    /** @copydoc AudioRecording::Prepare */
    Result<AudioRecording> AudioRecording::Prepare(const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                   const std::uint64_t maximumFrames) {
        using Output = Result<AudioRecording>;
        const auto shape = PrepareWaveShape(source.format, maximumFrames, MaximumRecordingBytes);
        if (!shape || shape->count > Audio::MaximumCoreAudioSourceChannels || !source.device.IsValid() || source.generation == 0 ||
            source.clockDomain == 0 || source.maximumFrames == 0 || source.maximumFrames > Audio::MaximumAudioCallbackFrames)
            return Output::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        try {
            return Output::Success(AudioRecording{source, std::vector<Audio::AudioSample>(maximumFrames * shape->count)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        }
    }

    AudioRecording::~AudioRecording() {
        Cancel();
    }

    /** @copydoc AudioRecording::Cancel */
    void AudioRecording::Cancel() noexcept {
        Security::SecureZero(std::as_writable_bytes(std::span{samples_}));
        samples_.clear();
    }

    /** @brief Checks the exact device, generation and sample format admitted for this recording. */
    bool AudioRecording::Admits(const Audio::Backend::AudioCaptureBufferDescriptor &source) const noexcept {
        return source.device == source_.device && source.generation == source_.generation && source.clockDomain == source_.clockDomain &&
               source.format == source_.format && source.maximumFrames == source_.maximumFrames;
    }

    /** @brief Rejects gaps, reordered samples and clock or frame evidence from a different take. */
    bool AudioRecording::Continues(const Audio::Backend::AudioCapturedBlock &block) const noexcept {
        return block.start.clockDomain == source_.clockDomain && block.start.nanoseconds >= latestStart_ &&
               block.sequence == sequence_ + 1 && block.firstFrame == frames_ && block.droppedFramesBefore == 0 && block.frames != 0 &&
               block.frames <= source_.maximumFrames;
    }

    /** @copydoc AudioRecording::Append */
    Result<void> AudioRecording::Append(const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                        const Audio::Backend::AudioCapturedBlock &block, std::span<const Audio::AudioSample> samples) {
        if (samples_.empty())
            return Result<void>::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
        const auto channels = source_.format.layout.orderedChannels.size();
        if (!Admits(source) || !Continues(block) || samples.size() != block.frames * channels ||
            !std::ranges::all_of(samples, [](const auto value) {
            return std::isfinite(value);
        })) {
            Cancel();
            return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        }
        auto available = std::span{samples_}.subspan(frames_ * channels);
        if (samples.size() > available.size()) {
            Cancel();
            return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceLimitExceeded));
        }
        std::ranges::copy(samples, available.begin());
        frames_ += block.frames;
        sequence_ = block.sequence;
        latestStart_ = block.start.nanoseconds;
        return Result<void>::Success();
    }

    /** @brief Reads every unread block of the stopped capture. Tail loss is terminal even without a following sample. */
    Result<void> AudioRecording::Drain(Audio::Backend::AudioCaptureBuffer &tail, const CancellationToken &cancellation) {
        const auto *source = tail.Descriptor();
        if (!source || !Admits(*source) || tail.DroppedFrames() != 0)
            return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        const auto channels = source_.format.layout.orderedChannels.size();
        std::vector<Audio::AudioSample> scratch(source_.maximumFrames * channels);
        ScrubBytes scrub{std::as_writable_bytes(std::span{scratch})};
        while (!cancellation.IsCancellationRequested()) {
            Audio::Backend::AudioCapturedBlock block;
            using enum Audio::Backend::AudioCaptureRead;
            switch (tail.TryRead(block, scratch)) {
                case Empty:
                    return Result<void>::Success();
                case Copied:
                    if (const auto appended = Append(*source, block, std::span{scratch}.first(block.frames * channels));
                        appended.HasError())
                        return appended;
                    break;
                case Cancelled:
                    return Result<void>::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
                case InsufficientSpace:
                    return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
            }
        }
        return Result<void>::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
    }

    /** @copydoc AudioRecording::Publish */
    Published AudioRecording::Publish(Audio::Backend::AudioCaptureBuffer tail, const AudioRecordingDestination &destination,
                                      ProjectAssetImportCommitter &publisher, const CancellationToken &cancellation) {
        try {
            auto recording = std::move(*this);
            if (cancellation.IsCancellationRequested() || recording.samples_.empty())
                return Published::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            if (!destination.path.String().ends_with(".wav"))
                return Published::Failure(MakeError(Audio::AudioErrors::SourceUnsupported));
            if (const auto drained = recording.Drain(tail, cancellation); drained.HasError())
                return Published::Failure(drained.ErrorValue());
            const auto shape = PrepareWaveShape(recording.source_.format, recording.frames_, MaximumRecordingBytes);
            if (!shape)
                return Published::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
            const auto samples = std::span{recording.samples_}.first(shape->frames * shape->count);
            auto wave = EncodeWave(*shape, recording.source_.format.sampleRate, samples, {.recording = cancellation});
            if (wave.HasError())
                return Published::Failure(wave.ErrorValue());
            recording.Cancel();
            return CommitWave(std::move(wave).Value(), destination, publisher, cancellation);
        } catch (const std::bad_alloc &) {
            Cancel();
            return Published::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Application::Internal
