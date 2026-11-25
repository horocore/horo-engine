#pragma once

/** @file Sdl3AudioBackend.h
 * @brief Build-tree-only SDL3 portability/reference audio backend.
 */

#include "Horo/Audio/Internal/AudioBackend.h"
#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"
#include "Horo/Audio/Internal/AudioCaptureMonitor.h"
#include "Horo/Audio/AudioStreamingService.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Audio::Backend {
    /** @brief Fixed construction facts; construction performs no SDL initialization or discovery. */
    struct Sdl3AudioBackendConfig final {
        AudioRuntimeId owner;
        std::uint64_t clockDomain{};
    };

    /** @brief Host-declared purpose scoped to one recording admission. */
    enum class Sdl3CapturePurpose : std::uint8_t {
        Unspecified,
        Recording,
        Monitoring,
        NetworkVoice,
        Speech,
        ConnectionTest
    };

    /** @brief Host decision about whether this capture may continue after focus loss. */
    struct Sdl3CaptureIntent final {
        Sdl3CapturePurpose purpose{Sdl3CapturePurpose::Unspecified};
        bool allowBackground{};
        std::optional<AudioCaptureMonitorRoute> monitor;
        bool operator==(const Sdl3CaptureIntent &) const noexcept = default;
    };

    /** @brief Current host admission for a single explicit device and capture generation.
     * @details The host owns purpose, consent and revocation. Backend verifies the exact request
     * and current observed platform permission. Zero authority revision is ungranted.
     */
    struct Sdl3CaptureGrant final {
        AudioCaptureDeviceId device;
        std::uint64_t discoveryRevision{};
        std::uint64_t generation{};
        std::uint64_t authorityRevision{};
        Sdl3CaptureIntent intent;
        bool foreground{}; /**< Current host focus observation. */
    };

    /** @brief One bounded consumer copy and the latest observed ring overflow count. */
    struct Sdl3CaptureReadout final {
        AudioCaptureRead status{};
        std::uint64_t droppedFrames{};
        std::optional<AudioCapturedBlock> latestDrop; /**< Timing only. May be empty while a callback reports overflow. */
    };

    /** @brief Callback evidence for the current capture generation. */
    enum class Sdl3CaptureActivityState : std::uint8_t {
        Inactive,
        AwaitingFrames,
        Capturing,
        Faulted
    };

    /** @brief Privacy-safe recording indicator payload. Contains no PCM or native device identity. */
    struct Sdl3CaptureActivity final {
        Sdl3CaptureActivityState state{Sdl3CaptureActivityState::Inactive};
        std::uint64_t generation{};
        Sdl3CapturePurpose purpose{Sdl3CapturePurpose::Unspecified};
        AudioSample peakLevel{}; /**< Latest normalized peak for recording or connection-test capture. */
    };

    /** @brief Result of an explicit request to the platform permission owner. */
    enum class Sdl3CapturePermissionRequest : std::uint8_t {
        Requested,
        AlreadyPending,
        DecisionAvailable
    };

#if defined(__APPLE__)
    /** @brief Current macOS permission facts without opening or initializing the SDL audio subsystem. */
    struct Sdl3MacCapturePermission final {
        AudioCapturePermission permission{AudioCapturePermission::Unknown};
        bool pending{};
    };

    /** @brief Returns macOS permission facts for the current package. */
    [[nodiscard]] Sdl3MacCapturePermission ObserveSdl3MacCapturePermission() noexcept;
    /** @brief Submits an explicit main-thread user request without constructing a device backend. */
    [[nodiscard]] Result<Sdl3CapturePermissionRequest> RequestSdl3MacCapturePermission();
#endif

    /** @brief Backend-local facts; parent AudioRuntime state remains control-owned. */
    enum class Sdl3AudioBackendState : std::uint8_t {
        Closed = 0,
        Opened = 1,
        Priming = 2,
        Rendering = 3,
        Quiescing = 4,
        Quiesced = 5,
        Stopped = 6
    };

    /**
     * @brief Equal-peer SDL3 output adapter with private native identity and conversion state.
     *
     * Begin and AdvanceControl run on one declared control thread. SDL invokes only the retained
     * Horo RenderPort on its audio thread; that path performs no Horo allocation, blocking, logging,
     * discovery or lifecycle policy. Stop detaches and destroys the stream before reporting success.
     */
    class Sdl3AudioBackend final : public AudioBackend {
    public:
        Sdl3AudioBackend(const Sdl3AudioBackend &) = delete;
        Sdl3AudioBackend &operator=(const Sdl3AudioBackend &) = delete;
        Sdl3AudioBackend(Sdl3AudioBackend &&) = delete;
        Sdl3AudioBackend &operator=(Sdl3AudioBackend &&) = delete;
        ~Sdl3AudioBackend() override;

        /** @copydoc AudioBackend::Kind */
        [[nodiscard]] AudioBackendKind Kind() const noexcept override;
        /** @copydoc AudioBackend::Owner */
        [[nodiscard]] AudioRuntimeId Owner() const noexcept override;

        /** @brief Return the current backend-local lifecycle fact. @return Current SDL3 adapter state. */
        [[nodiscard]] Sdl3AudioBackendState State() const noexcept;

        /** @copydoc AudioBackend::Begin */
        [[nodiscard]] Result<OperationId> Begin(const Request &request, const AudioMonotonicTimestamp &deadline) override;
        /** @copydoc AudioBackend::Poll */
        [[nodiscard]] Result<std::optional<Completion>> Poll(const OperationId &operation) override;

        /**
         * @brief Execute one admitted native control operation.
         * @return Success when its outcome is published or remains asynchronously pending.
         */
        [[nodiscard]] Result<void> AdvanceControl();

        /** @brief Refreshes SDL's recording inventory on the backend control thread without opening a device.
         * @return Complete catalog and typed changes from the last successful refresh, or a typed error.
         * @details SDL exposes the system recording route through Multimedia. Other roles remain unavailable.
         * The Core Audio driver can observe authorization without requesting it. The host chooses when to
         * refresh. This call does not consume SDL's shared event queue or start capture.
         */
        [[nodiscard]] Result<AudioCaptureDiscovery> DiscoverCapture() override;

        /** @brief Asks the platform for microphone access in response to an explicit host user action.
         * @return Request state or typed failure. A decision is subsequently read through DiscoverCapture.
         * @pre Called on the application main thread. This does not open a device or grant a capture purpose.
         */
        [[nodiscard]] Result<Sdl3CapturePermissionRequest> RequestCapturePermission();

        /** @brief Opens one explicitly selected recording endpoint on Audio control.
         * @param descriptor Immutable format, owner, clock and queue bounds.
         * @param intent Capture purpose and requested background policy.
         * @param grant Exact host permission decision for this request.
         * @return Success once callback admission opens, or a typed error. No prompt is requested.
         */
        [[nodiscard]] Result<void> StartCapture(const AudioCaptureBufferDescriptor &descriptor, const Sdl3CaptureIntent &intent,
                                                const Sdl3CaptureGrant &grant);

        /** @brief Reconciles current consent and native health without requiring the consumer to read PCM.
         * @param generation Exact active capture generation.
         * @param grant The current host grant with current focus evidence, or null after consent withdrawal.
         * @return Success while admitted or typed failure. Invalidated admission detaches and retires this generation.
         */
        [[nodiscard]] Result<void> ReconcileCapture(std::uint64_t generation, const std::optional<Sdl3CaptureGrant> &grant);

        /** @brief Returns capture activity on Audio control without reading PCM or querying the OS.
         * @return Inactive when no stream is owned. AwaitingFrames, Capturing and Faulted denote an open stream.
         * @details Reconcile current consent and native health independently before presenting the status.
         * A callback fault does not imply native detachment. Stop or reconciliation retires the generation.
         */
        [[nodiscard]] Sdl3CaptureActivity CaptureActivity() const noexcept;

        /** @brief Copies one recording or speech block after checking current host admission and native health on Audio control.
         * @param generation Exact active capture generation.
         * @param grant Current host admission for this generation, or empty if withdrawn.
         * @param block Destination written only on a successful copy. Timestamps use an SDL host-clock estimate.
         * @param pcm Consumer-owned destination for interleaved float PCM.
         * @return Copy status and loss count, or typed stale session, permission loss, device loss or callback fault.
         * A withdrawn grant retires the stream before copying. Errors leave both destinations untouched.
         * @details Connection testing reports its level through CaptureActivity and never grants PCM access.
         * Monitoring requires ReadMonitoring so raw PCM cannot bypass its gain and queue-age policy.
         */
        [[nodiscard]] Result<Sdl3CaptureReadout> ReadCapture(std::uint64_t generation, const std::optional<Sdl3CaptureGrant> &grant,
                                                             AudioCapturedBlock &block, std::span<AudioSample> pcm);

        /** @brief Copies one timestamped PCM block for an admitted network-voice capture on Audio control.
         * @param generation Exact active capture generation.
         * @param grant Current host network-voice admission, or empty to retire the generation and discard queued PCM.
         * @param block Destination written only on a successful copy.
         * @param pcm Consumer-owned buffer for interleaved float PCM.
         * @return Copy status and capture loss evidence, or a typed stale, consent or device failure.
         * @details The consumer owns encoding and transport. Recording and speech generations cannot be read through this endpoint.
         */
        [[nodiscard]] Result<Sdl3CaptureReadout> ReadNetworkVoiceCapture(std::uint64_t generation,
                                                                        const std::optional<Sdl3CaptureGrant> &grant,
                                                                        AudioCapturedBlock &block, std::span<AudioSample> pcm);

        /** @brief Reads gain-limited local monitor PCM under the current exact consent and output route.
         * @param generation Exact active capture generation. @param grant Refreshed host admission for this route.
         * @param block Metadata written on successful copy. @param pcm Owner-provided local mixer input storage.
         * @return Bounded copy status or typed consent/output health failure. Expired PCM is discarded.
         * @details Does not connect endpoints or invoke an output callback. The application owns the published mixer source.
         */
        [[nodiscard]] Result<AudioMonitorRead> ReadMonitoring(std::uint64_t generation, const Sdl3CaptureGrant &grant,
                                                              AudioCapturedBlock &block, std::span<AudioSample> pcm);

        /** @brief Binds a prepared live playback source to an already admitted monitor generation.
         * @param grant Exact current monitor admission, including the pinned output and mixer bus.
         * @param service Host-retained stream service used by that output's normal voice and mixer.
         * @param stream Exact empty live source prepared for the admitted mixer bus before output activation.
         * @return Success or a typed scope, format or capacity failure without consuming the source.
         * @details Capture retirement stops this source. The host retires its storage after detaching the voice.
         */
        [[nodiscard]] Result<void> BindMonitoringSource(const Sdl3CaptureGrant &grant,
                                                         const std::shared_ptr<AudioStreamingService> &service, AudioStreamHandle stream);

        /** @brief Pumps one current, gain-limited microphone block to the bound live mixer source on control.
         * @param grant Exact refreshed monitor admission.
         * @param pcm Caller-owned, preallocated scratch; copied microphone samples are cleared before returning.
         * @return Bounded read or typed route, health or queue-capacity failure. A full playback queue drops that block.
         */
        [[nodiscard]] Result<AudioMonitorRead> PumpMonitoringSource(const Sdl3CaptureGrant &grant, std::span<AudioSample> pcm);

        /** @brief Stops an explicitly approved recording, transferring its private unread tail to the caller.
         * @param generation Exact active capture generation.
         * @param grant Current owner admission for the same recording. Revocation discards PCM.
         * @return Detached buffer, or typed stale, health or permission failure.
         * @pre Audio control lane. The caller owns the buffer through finalization or discards it.
         */
        [[nodiscard]] Result<AudioCaptureBuffer> FinishRecording(std::uint64_t generation, const Sdl3CaptureGrant &grant);

        /** @brief Seals an admitted speech generation and transfers its unread PCM on the control lane.
         * @param generation Exact active speech capture generation.
         * @param grant Current speech admission. Withdrawal or loss discards the queue.
         * @return Detached gap-free tail for the speech worker, or typed stale, health, overflow or permission failure.
         */
        [[nodiscard]] Result<AudioCaptureBuffer> FinishSpeech(std::uint64_t generation, const Sdl3CaptureGrant &grant);

        /** @brief Closes one generation, including host-driven revocation. Repeated stop for that generation succeeds.
         * @param generation Previously started capture generation.
         * @return Success after native detachment or a typed stale-generation error.
         */
        [[nodiscard]] Result<void> StopCapture(std::uint64_t generation);

        /** @copydoc AudioBackend::AcknowledgeCompletion */
        [[nodiscard]] Result<void> AcknowledgeCompletion(const OperationId &operation) override;
        /** @copydoc AudioBackend::Cancel */
        [[nodiscard]] Result<CancelDisposition> Cancel(const OperationId &operation) override;
        /** @copydoc AudioBackend::CommitRendering */
        [[nodiscard]] Result<void> CommitRendering(const AudioDeviceEpoch &epoch) override;
        /** @copydoc AudioBackend::DrainEvents */
        [[nodiscard]] std::size_t DrainEvents(std::span<Event> output) noexcept override;
        /** @copydoc AudioBackend::DrainSafetyViolations */
        [[nodiscard]] AudioCallbackViolationDrain DrainSafetyViolations(std::span<AudioCallbackViolation> output) noexcept override;

    private:
        /** @brief Reads monitored input for the sole admitted consumer. */
        [[nodiscard]] Result<AudioMonitorRead> ReadMonitoringBlock(std::uint64_t generation, const Sdl3CaptureGrant &grant,
                                                                   AudioCapturedBlock &block, std::span<AudioSample> pcm);
        /** @brief Shares validated PCM copying while keeping network-voice and local capture consumers separate. */
        [[nodiscard]] Result<Sdl3CaptureReadout> ReadBufferedCapture(std::uint64_t generation, const std::optional<Sdl3CaptureGrant> &grant,
                                                                     AudioCapturedBlock &block, std::span<AudioSample> pcm,
                                                                     std::optional<Sdl3CapturePurpose> expectedPurpose);
        [[nodiscard]] Result<AudioCaptureBuffer> FinishCapture(std::uint64_t generation, const Sdl3CaptureGrant &grant,
                                                               Sdl3CapturePurpose purpose);
        struct Impl;
        friend Result<std::unique_ptr<Sdl3AudioBackend>> CreateSdl3AudioBackend(const Sdl3AudioBackendConfig &config);
        explicit Sdl3AudioBackend(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /**
     * @brief Construct an inert SDL3 peer without initializing SDL or opening a device.
     * @param config Fixed runtime owner and host monotonic clock domain.
     * @return Owned backend or a stable validation/allocation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<Sdl3AudioBackend>> CreateSdl3AudioBackend(const Sdl3AudioBackendConfig &config);
}  // namespace Horo::Audio::Backend
