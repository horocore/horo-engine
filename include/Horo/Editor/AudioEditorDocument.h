#pragma once

/** @file AudioEditorDocument.h
 * @brief Shared audio authoring document identity, preview fencing and transport ownership.
 */

#include "Horo/Audio/AudioFrontend.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <array>

namespace Horo::Editor {
    namespace AudioEditorDocumentErrors {
        extern const ErrorCodeDescriptor InvalidIdentity;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor PreviewUnavailable;
        extern const ErrorCodeDescriptor PreviewBusy;
        extern const ErrorCodeDescriptor Unfocused;
    }  // namespace AudioEditorDocumentErrors
    /** @brief Lifecycle of authored identity, independent of asynchronous output teardown. */
    enum class AudioEditorDocumentPhase : std::uint8_t {
        Open,
        Closing,
        Closed
    };

    /** @brief Owned source/preview correlation and actual Audio output lifecycle. */
    struct AudioEditorDocumentSnapshot final {
        DocumentIdentity identity;
        std::uint64_t sourceRevision{};
        std::uint64_t previewRevision{};
        bool focused{};
        AudioEditorDocumentPhase phase{AudioEditorDocumentPhase::Closed};
        std::optional<Audio::AudioFrontendSnapshot> preview;
        std::size_t retainedTransportResults{};
    };

    /**
     * @brief Shared owner for an audio asset document's isolated production-path preview.
     * @details Runs on the editor/audio control owner thread. Clip, mixer, recording and graph
     * tools retain their existing source/history authority; this foundation never writes source,
     * dirty state or preview overrides into assets. The host issues the Asset document identity,
     * captures a source revision, prepares an isolated AudioFrontend and marshals its owner calls.
     * Widgets receive snapshots and revision-fenced commands, never device/backend access.
     * Reload closes the previous preview before a new capture is attached. Tab/project close
     * retains the frontend until Pump observes complete output/stream retirement; it never
     * mistakes a queue admission, silence or a timeout for native detachment.
     */
    class AudioEditorDocument final {
    public:
        /** @brief Opens a host-issued audio Asset document without starting a device.
         * @param identity Current document instance and portable Asset source key.
         * @param sourceRevision Nonzero revision of the authoring owner's immutable source capture.
         * @return Independent document session or typed identity error.
         */
        [[nodiscard]] static Result<AudioEditorDocument> Open(DocumentIdentity identity, std::uint64_t sourceRevision);
        AudioEditorDocument(AudioEditorDocument &&other) noexcept;
        AudioEditorDocument &operator=(AudioEditorDocument &&) = delete;
        AudioEditorDocument(const AudioEditorDocument &) = delete;
        AudioEditorDocument &operator=(const AudioEditorDocument &) = delete;
        /** @brief Releases a closed/never-started document on its owner thread.
         * @pre Pump Close to Closed before destroying an attached preview, including tab/project shutdown.
         */
        ~AudioEditorDocument();

        /** @brief Adopts a detached prepared preview for the current captured source revision.
         * @param expectedRevision Current authoring revision; stale candidates retain caller ownership.
         * @param preview Explicit host-composed isolated frontend, consumed only on successful adoption.
         * @return Success or stale/closed/invalid/busy error, preserving the existing owner.
         */
        [[nodiscard]] Result<void> AttachPreview(std::uint64_t expectedRevision, std::unique_ptr<Audio::AudioFrontend> &preview);
        /** @brief Starts the attached captured preview through its ordinary Audio output lifecycle.
         * @param expectedRevision Current source revision. @param deadline Host Audio monotonic deadline.
         * @return Success or typed stale/closed/unavailable/output failure.
         */
        [[nodiscard]] Result<void> StartPreview(std::uint64_t expectedRevision, Audio::AudioMonotonicTimestamp deadline);
        /** @brief Routes one transport command through the production frontend FIFO.
         * @param expectedRevision Captured source revision. @param control Typed start/stop/pause/resume/seek/etc. intent.
         * @return Admission/retry or typed stale/closed/unfocused failure; application is callback-owned.
         */
        [[nodiscard]] Result<Audio::AudioCommandAdmission> Transport(std::uint64_t expectedRevision,
                                                                     Audio::AudioVoiceControlRequest control);
        /** @brief Copies and acknowledges retained terminal transport results, including after reload/close.
         * @param output Caller-owned storage; results carry their originating runtime and admission sequence.
         * @return Number of results copied. Undrained old-preview results block replacement admission;
         * device/voice/stream retirement does not depend on the presentation consumer.
         */
        [[nodiscard]] std::size_t DrainTransportResults(std::span<Audio::AudioFrontendOperationResult> output) noexcept;
        /** @brief Changes document interaction focus without native device calls or implicit playback.
         * @param expectedRevision Current source revision. @param focused Whether host routing selects this document.
         * @return Success or typed stale/closed error. Focus loss disables new playback intent, but Stop/Pause remain available.
         */
        [[nodiscard]] Result<void> SetFocused(std::uint64_t expectedRevision, bool focused);
        /** @brief Fences an externally committed authoring reload and closes the old preview.
         * @param expectedRevision Current source revision. @param nextRevision Strictly newer nonzero authoring revision.
         * @return Success or typed stale/invalid error; authored data/history remain with their owner.
         */
        [[nodiscard]] Result<void> Reload(std::uint64_t expectedRevision, std::uint64_t nextRevision);
        /** @brief Requests document teardown and retains the preview until native/worker cleanup completes. */
        void Close() noexcept;
        /** @brief Advances asynchronous preview startup/teardown on the control owner.
         * @param deadline Host Audio monotonic deadline for the next lifecycle operation.
         * @return Success or original preview failure; ownership remains retained on incomplete cleanup.
         */
        [[nodiscard]] Result<void> Pump(Audio::AudioMonotonicTimestamp deadline);
        /** @brief Returns owned document and preview facts without mutable borrows. */
        [[nodiscard]] AudioEditorDocumentSnapshot Snapshot() const;

    private:
        /** @brief Retains validated host identity and source revision without opening output. */
        AudioEditorDocument(DocumentIdentity identity, std::uint64_t revision) noexcept;
        /** @brief Checks source correlation before any state change or runtime call. */
        [[nodiscard]] Result<void> CheckRevision(std::uint64_t expectedRevision) const;
        DocumentIdentity identity_;
        std::uint64_t revision_{};
        std::uint64_t previewRevision_{};
        bool focused_{};
        AudioEditorDocumentPhase phase_{AudioEditorDocumentPhase::Open};
        std::unique_ptr<Audio::AudioFrontend> preview_;
        std::array<Audio::AudioFrontendOperationResult, Audio::MaximumAudioFrontendOperations> retiredResults_;
        std::size_t retiredResultCount_{};
    };
}  // namespace Horo::Editor
