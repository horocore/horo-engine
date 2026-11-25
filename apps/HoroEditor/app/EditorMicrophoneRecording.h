#pragma once

/** @file EditorMicrophoneRecording.h @brief One detached editor recording and its asynchronous project publication. */
#include "Horo/Application/Internal/AudioRecording.h"

#include <future>
#include <thread>

namespace Horo::Editor {
    /** @brief Reserves a fresh recording identity for a canonical project WAV path.
     * @param root Active project root. @param path Portable destination path under assets.
     * @return Owned recording destination, or empty when validation or identity allocation fails.
     */
    [[nodiscard]] std::optional<Application::Internal::AudioRecordingDestination> RecordingTarget(const std::filesystem::path &root,
                                                                                                  std::string_view path);

    /** @brief A durable recording adopted by the active project registry. */
    struct EditorPublishedRecording final {
        Assets::AssetId id;
        std::string path;
    };

    /** @brief Owns bounded PCM accumulated on the control lane before transfer to the project publisher. */
    class EditorRecordingTake final {
    public:
        /** @brief Prepares one take and scratch space before opening the microphone. */
        [[nodiscard]] static Result<EditorRecordingTake> Prepare(const Audio::Backend::AudioCaptureBufferDescriptor &capture,
                                                                 Application::Internal::AudioRecordingDestination destination);
        EditorRecordingTake(EditorRecordingTake &&) = default;
        EditorRecordingTake &operator=(EditorRecordingTake &&) = delete;
        ~EditorRecordingTake();

        /** @brief Copies a complete ordered callback block into privately owned storage and scrubs the intermediate copy. */
        [[nodiscard]] Result<void> Append(const Audio::Backend::AudioCapturedBlock &block);
        Audio::Backend::AudioCaptureBufferDescriptor capture;
        Application::Internal::AudioRecordingDestination destination;
        Application::Internal::AudioRecording recording;
        std::vector<Audio::AudioSample> scratch;

    private:
        EditorRecordingTake(Audio::Backend::AudioCaptureBufferDescriptor descriptor,
                            Application::Internal::AudioRecordingDestination target, Application::Internal::AudioRecording accumulation);
    };

    /** @brief Publishes only detached PCM. The worker never borrows the live editor registry or the microphone. */
    class EditorRecordingPublication final {
    public:
        EditorRecordingPublication(ProjectMutationCoordinator &mutations, DurableFileSystem &files, Assets::AssetRegistry &registry);
        ~EditorRecordingPublication();

        /** @brief Starts one background save under the normal project mutation lease. */
        [[nodiscard]] bool Start(EditorRecordingTake take, Audio::Backend::AudioCaptureBuffer tail);
        /** @brief Cancels pending work and suppresses adoption after project retirement. */
        void Cancel() noexcept;
        /** @brief Returns whether a previous save still occupies the worker slot. */
        [[nodiscard]] bool Busy() const noexcept;
        /** @brief Adopts a finished asset in the live registry on its owner lane. Empty while still busy. */
        [[nodiscard]] std::optional<Result<EditorPublishedRecording>> Poll();

    private:
        ProjectMutationCoordinator &mutations_;
        DurableFileSystem &files_;
        Assets::AssetRegistry &registry_;
        std::optional<CancellationSource> cancellation_;
        std::future<Result<Assets::AssetRecord>> job_;
        std::jthread worker_;
    };
}  // namespace Horo::Editor
