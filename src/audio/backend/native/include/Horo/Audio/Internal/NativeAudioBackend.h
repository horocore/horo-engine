#pragma once

/** @file NativeAudioBackend.h
 * @brief Shared Horo lifecycle/PCM mechanism for independently composed native peers.
 */

#include "Horo/Audio/Internal/NativeAudioDriver.h"

#include <memory>

namespace Horo::Audio::Backend {
    /** @brief Fixed owner and steady-clock domain; no discovery or native initialization at construction. */
    struct NativeAudioBackendConfig final {
        AudioRuntimeId owner;
        std::uint64_t clockDomain{};
    };

    /**
     * @brief Backend-neutral protocol execution; concrete native drivers remain equal peers.
     *
     * Begin copies intent; AdvanceControl executes one bounded native control step. The declared control
     * thread also owns native apartment/run-loop acquisition. Native callbacks borrow only immutable
     * format/scratch/port and lock-free phase/fact mailboxes. Shutdown retains these through Stop proof.
     * This utility selects no backend and is not an SDL/Null/native implementation used as another's base.
     */
    class NativeAudioBackend final : public AudioBackend {
    public:
        NativeAudioBackend(const NativeAudioBackend &) = delete;
        NativeAudioBackend &operator=(const NativeAudioBackend &) = delete;
        ~NativeAudioBackend() override;
        /** @copydoc AudioBackend::Kind */
        [[nodiscard]] AudioBackendKind Kind() const noexcept override;
        /** @copydoc AudioBackend::Owner */
        [[nodiscard]] AudioRuntimeId Owner() const noexcept override;
        /** @copydoc AudioBackend::DiscoverCapture */
        [[nodiscard]] Result<AudioCaptureDiscovery> DiscoverCapture() override;
        /** @copydoc AudioBackend::Begin */
        [[nodiscard]] Result<OperationId> Begin(const Request &request, const AudioMonotonicTimestamp &deadline) override;
        /** @copydoc AudioBackend::CommitRendering */
        [[nodiscard]] Result<void> CommitRendering(const AudioDeviceEpoch &epoch) override;
        /** @copydoc AudioBackend::ReadPlaybackLatency */
        [[nodiscard]] std::optional<PlaybackLatencyEstimate> ReadPlaybackLatency(const AudioDeviceEpoch &epoch) noexcept override;
        /** @copydoc AudioBackend::Cancel */
        [[nodiscard]] Result<CancelDisposition> Cancel(const OperationId &operation) override;
        /** @copydoc AudioBackend::Poll */
        [[nodiscard]] Result<std::optional<Completion>> Poll(const OperationId &operation) override;
        /** @copydoc AudioBackend::AcknowledgeCompletion */
        [[nodiscard]] Result<void> AcknowledgeCompletion(const OperationId &operation) override;
        /** @copydoc AudioBackend::DrainEvents */
        [[nodiscard]] std::size_t DrainEvents(std::span<Event> output) noexcept override;
        /** @copydoc AudioBackend::DrainSafetyViolations */
        [[nodiscard]] AudioCallbackViolationDrain DrainSafetyViolations(std::span<AudioCallbackViolation> output) noexcept override;
        /** @brief Execute one admitted native control step without waiting on callback progress. @return Progress/query result. */
        [[nodiscard]] Result<void> AdvanceControl();

    private:
        struct Impl;
        explicit NativeAudioBackend(std::unique_ptr<Impl> implementation) noexcept;
        friend Result<std::unique_ptr<NativeAudioBackend>> CreateNativeAudioBackend(NativeAudioBackendConfig,
                                                                                    std::unique_ptr<NativeAudioDriver>);
        std::unique_ptr<Impl> impl_;
    };

    /**
     * @brief Compose one inert concrete driver with Horo's common lifecycle mechanism.
     * @param config Fixed runtime identity and clock domain shared with the driver.
     * @param driver Unique native peer; construction and Kind perform no native initialization.
     * @return Unique protocol owner or typed validation/allocation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<NativeAudioBackend>> CreateNativeAudioBackend(NativeAudioBackendConfig config,
                                                                                       std::unique_ptr<NativeAudioDriver> driver);
}  // namespace Horo::Audio::Backend
