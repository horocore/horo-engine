#pragma once

/** @file AudioRecording.h @brief Bounded capture accumulation and publication as an ordinary project audio asset. */
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Audio/Internal/AudioCaptureBuffer.h"

namespace Horo::Application::Internal {
    /** @brief Host-reserved asset identity and project destination for one explicit recording save. */
    struct AudioRecordingDestination final {
        std::string operationId;
        std::filesystem::path projectRoot;
        ProjectPath path;
        Assets::AssetId id;
    };

    /** @brief Exported properties validated by the ordinary source importer. */
    struct PublishedAudioRecording final {
        std::uint64_t frames{};
        std::uint64_t durationNanoseconds{};
        std::uint32_t sampleRate{};
    };

    /** @brief One private detached recording accumulator. All operations run on a single non-realtime owner lane. */
    class AudioRecording final {
    public:
        /** @brief Reserves bounded scratch storage for one admitted capture identity and a finite frame budget. */
        [[nodiscard]] static Result<AudioRecording> Prepare(const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                            std::uint64_t maximumFrames);
        AudioRecording(AudioRecording &&) = default;
        AudioRecording &operator=(AudioRecording &&) = delete;
        ~AudioRecording();

        /** @brief Copies one complete ordered block. A gap, wrong generation or exhausted budget discards the recording. */
        [[nodiscard]] Result<void> Append(const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                          const Audio::Backend::AudioCapturedBlock &block, std::span<const Audio::AudioSample> samples);

        /** @brief Drains a normally stopped capture, validates WAVE, then commits it with an ordinary identity sidecar.
         * @param tail Exact buffer transferred after the capture callback detached, with no callback or reader remaining.
         * @param destination Explicit authorized asset target for this save.
         * @param publisher Host-owned project publication authority.
         * @param files Filesystem used to check capacity before starting publication.
         * @param cancellation Stop token observed before publication and during validation.
         * @return Published audio metadata or typed failure. Inspect publisher's receipt if publication was attempted.
         * @details This is a one-shot operation. All failure paths discard private PCM. No intermediate recording file is retained.
         */
        [[nodiscard]] Result<PublishedAudioRecording> Publish(Audio::Backend::AudioCaptureBuffer tail,
                                                              const AudioRecordingDestination &destination,
                                                              ProjectAssetImportCommitter &publisher,
                                                              const DurableFileSystem &files,
                                                              const CancellationToken &cancellation);

        /** @brief Irreversibly discards this accumulation and scrubs its private samples. */
        void Cancel() noexcept;

    private:
        AudioRecording(Audio::Backend::AudioCaptureBufferDescriptor source, std::vector<Audio::AudioSample> samples)
            : source_(std::move(source)), samples_(std::move(samples)) {}

        [[nodiscard]] Result<void> Drain(Audio::Backend::AudioCaptureBuffer &tail, const CancellationToken &cancellation);
        [[nodiscard]] bool Admits(const Audio::Backend::AudioCaptureBufferDescriptor &source) const noexcept;
        [[nodiscard]] bool Continues(const Audio::Backend::AudioCapturedBlock &block) const noexcept;

        Audio::Backend::AudioCaptureBufferDescriptor source_;
        std::vector<Audio::AudioSample> samples_;
        std::uint64_t frames_{};
        std::uint64_t sequence_{};
        std::uint64_t latestStart_{};
    };
}  // namespace Horo::Application::Internal
