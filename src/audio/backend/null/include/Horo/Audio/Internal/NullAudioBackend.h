#pragma once

/** @file NullAudioBackend.h
 * @brief Build-tree-only deterministic NullAudio peer for headless and test composition.
 */

#include "Horo/Audio/AudioClock.h"
#include "Horo/Audio/Internal/AudioBackend.h"
#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Audio::Backend {
    /** @brief Fixed construction facts; no ambient backend registration or wall clock is consulted. */
    struct NullAudioBackendConfig final {
        AudioRuntimeId owner;
        std::uint64_t clockDomain{};
        std::uint64_t clockGeneration{1};
        std::uint64_t discontinuityRevision{1};
    };

    /** @brief Explicit deterministic control/device lifecycle exposed for bounded headless driving. */
    enum class NullAudioBackendState : std::uint8_t {
        Closed,
        Opened,
        Priming,
        Rendering,
        Quiescing,
        Quiesced,
        Stopped
    };

    /**
     * @brief Equal-peer NullAudio backend driven only by explicit control and callback steps.
     *
     * Control operations become observable only through AdvanceControl. AdvanceCallback invokes the
     * retained Horo render port with preallocated planar storage and advances an integer sample clock.
     * Neither path reads wall time, opens hardware, invents physical latency, or selects a fallback.
     */
    class NullAudioBackend final : public AudioBackend {
    public:
        NullAudioBackend(const NullAudioBackend &) = delete;
        NullAudioBackend &operator=(const NullAudioBackend &) = delete;
        NullAudioBackend(NullAudioBackend &&) = delete;
        NullAudioBackend &operator=(NullAudioBackend &&) = delete;
        ~NullAudioBackend() override = default;

        /** @brief Return the fixed peer kind. @return NullAudio. */
        [[nodiscard]] AudioBackendKind Kind() const noexcept override;
        /** @brief Return the fixed runtime owner. @return Construction-time generation. */
        [[nodiscard]] AudioRuntimeId Owner() const noexcept override;
        /** @copydoc AudioBackend::Begin */
        [[nodiscard]] Result<OperationId> Begin(const Request &request, const AudioMonotonicTimestamp &deadline) override;
        /** @copydoc AudioBackend::CommitRendering */
        [[nodiscard]] Result<void> CommitRendering(const AudioDeviceEpoch &epoch) override;
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

        /**
         * @brief Complete one admitted control step without consulting wall time.
         * @return Success, including a Quiesce step awaiting callback acknowledgement, or a typed failure.
         */
        [[nodiscard]] Result<void> AdvanceControl();

        /**
         * @brief Execute one exact negotiated callback block and advance deterministic sample time.
         * @return New clock correlation, or a typed lifecycle/render failure without a false acknowledgement.
         */
        [[nodiscard]] Result<AudioClockCorrelationSnapshot> AdvanceCallback() noexcept;

        /**
         * @brief Queue a synthetic device-loss fact for deterministic recovery tests.
         * @param cause Explicit backend-neutral loss cause.
         * @return Success or a lifecycle/capacity error; no runtime transition is committed.
         */
        [[nodiscard]] Result<void> InjectDeviceLoss(DeviceLossCause cause) noexcept;

        /**
         * @brief Queue a synthetic interruption edge for deterministic policy tests.
         * @param state Explicit began/ended edge.
         * @return Success or a lifecycle/capacity error; no automatic resume occurs.
         */
        [[nodiscard]] Result<void> InjectInterruption(InterruptionState state) noexcept;

        /** @brief Return current backend-local lifecycle fact. @return Explicit state, never parent runtime state. */
        [[nodiscard]] NullAudioBackendState State() const noexcept;
        /** @brief Return the deterministic sample cursor. @return Frames advanced in the current callback epoch. */
        [[nodiscard]] std::uint64_t SampleFrame() const noexcept;

    private:
        friend Result<std::unique_ptr<NullAudioBackend>> CreateNullAudioBackend(const NullAudioBackendConfig &config);
        explicit NullAudioBackend(const NullAudioBackendConfig &config) noexcept;

        [[nodiscard]] Result<void> ValidateRequest(const Request &request) const;
        [[nodiscard]] Result<void> ValidateOpenRequest(const Open &request) const;
        [[nodiscard]] Result<void> ValidateLifecycleRequest(const Request &request) const;
        [[nodiscard]] Result<void> ValidateStartRequest(const Start &request) const;
        [[nodiscard]] Result<void> ValidateQuiesceRequest(const Quiesce &request) const;
        [[nodiscard]] Result<void> ValidateStopRequest(const Stop &request) const;
        [[nodiscard]] Result<void> Apply(const Request &request);
        void ApplyOpen(const Open &request, OperationId operation);
        [[nodiscard]] Result<void> PushEvent(const Event &event) noexcept;
        [[nodiscard]] RenderPhase CurrentRenderPhase() const noexcept;
        [[nodiscard]] Result<void> ValidateRenderResult(const RenderResult &result) noexcept;
        [[nodiscard]] Result<void> AdvanceSampleClock() noexcept;
        [[nodiscard]] Result<void> PublishCallbackTransition(const AudioMonotonicTimestamp &completedAt) noexcept;
        [[nodiscard]] AudioClockCorrelationSnapshot ClockSnapshot() const noexcept;

        static constexpr std::size_t MaximumEvents = 64;
        static constexpr std::size_t MaximumInjectedEvents = 60;

        /** @brief One deterministic callback clock cursor; reset and advanced as a unit per render epoch. */
        struct ClockCursor final {
            std::uint64_t sampleFrame{};
            std::uint64_t nanoseconds{};
            std::uint64_t remainder{};
        };

        NullAudioBackendConfig config_;
        AudioDeviceId device_;
        NullAudioBackendState state_{NullAudioBackendState::Closed};
        std::uint64_t nextOperationSequence_{1};
        std::optional<OperationId> pendingOperation_;
        std::optional<Request> pendingRequest_;
        std::optional<Completion> completion_;
        AudioDeviceEpoch epoch_;
        AudioProcessingFormat format_;
        std::uint32_t callbackFrames_{};
        std::size_t planeStrideSamples_{};
        RenderPort render_;
        AudioCallbackWatchdog watchdog_;
        bool ready_{};
        ClockCursor clock_;
        std::vector<AudioSample> samples_;
        std::vector<AudioSample *> planes_;
        std::array<Event, MaximumEvents> events_{};
        std::size_t eventCount_{};
    };

    /**
     * @brief Construct one inert NullAudio peer without probing or registering ambient services.
     * @param config Valid runtime owner and deterministic clock identities.
     * @return Unique backend owner or a typed validation/allocation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<NullAudioBackend>> CreateNullAudioBackend(const NullAudioBackendConfig &config);
}  // namespace Horo::Audio::Backend
