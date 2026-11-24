#pragma once

/** @file Sdl3AudioBackend.h
 * @brief Build-tree-only SDL3 portability/reference audio backend.
 */

#include "Horo/Audio/Internal/AudioBackend.h"
#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"
#include "Horo/Audio/Internal/AudioCaptureBuffer.h"

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

    /** @brief Current host admission for a single explicit device and capture generation.
     * @details The host owns purpose, consent and revocation. Backend verifies the exact request
     * and current observed platform permission. Zero authority revision is ungranted.
     */
    struct Sdl3CaptureGrant final {
        AudioCaptureDeviceId device;
        std::uint64_t discoveryRevision{};
        std::uint64_t generation{};
        std::uint64_t authorityRevision{};
    };

    /** @brief One bounded consumer copy and the latest observed ring overflow count. */
    struct Sdl3CaptureReadout final {
        AudioCaptureRead status{};
        std::uint64_t droppedFrames{};
    };

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

        /** @brief Opens one explicitly selected recording endpoint on Audio control.
         * @param descriptor Immutable format, owner, clock and queue bounds.
         * @param grant Exact host permission decision for this request.
         * @return Success once callback admission opens, or a typed error. No prompt is requested.
         */
        [[nodiscard]] Result<void> StartCapture(const AudioCaptureBufferDescriptor &descriptor, const Sdl3CaptureGrant &grant);

        /** @brief Copies one captured block after checking native health and permission on Audio control.
         * @param generation Exact active capture generation.
         * @param block Destination written only on a successful copy. Timestamps use an SDL host-clock estimate.
         * @param pcm Consumer-owned destination for interleaved float PCM.
         * @return Copy status and loss count, or typed stale session, permission loss, device loss or callback fault.
         */
        [[nodiscard]] Result<Sdl3CaptureReadout> ReadCapture(std::uint64_t generation, AudioCapturedBlock &block,
                                                             std::span<AudioSample> pcm);

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
