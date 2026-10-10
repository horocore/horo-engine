#pragma once

/** @file AudioFrontend.h
 * @brief Audio-owned bounded playback and output lifecycle shared by runtime and editor preview.
 */

#include "Horo/Audio/AudioCommandStaging.h"
#include "Horo/Audio/AudioDeviceTiming.h"
#include "Horo/Audio/AudioVoiceControls.h"

#include <memory>
#include <optional>

namespace Horo::Audio {
    namespace Internal {
        struct AudioFrontendComposition;
    }

    /** @brief Control-owned lifecycle; queue admission never implies audible application. */
    enum class AudioFrontendPhase : std::uint8_t {
        Prepared,
        Opening,
        Starting,
        Active,
        Closing,
        Closed,
        Retained
    };

    inline constexpr std::size_t MaximumAudioFrontendOperations = 64;
    /** @brief Observed command application; independent of voice lifetime/native detachment. */
    enum class AudioFrontendOperationDisposition : std::uint8_t {
        Applied,
        Rejected,
        Cancelled
    };

    /** @brief One retained terminal transport result, identified across replacement runtimes. */
    struct AudioFrontendOperationResult final {
        AudioRuntimeId owner;
        std::uint64_t acceptedSequence{};
        AudioFrontendOperationDisposition disposition{AudioFrontendOperationDisposition::Cancelled};
        const ErrorCodeDescriptor *error{}; /**< Static Audio failure identity; populated only for Rejected. */
    };

    /** @brief Owned status with the original failure retained through cleanup. */
    struct AudioFrontendSnapshot final {
        AudioFrontendPhase phase{AudioFrontendPhase::Prepared};
        AudioRuntimeId owner;
        AudioVoiceHandle voice;
        AudioDeviceEpoch device;
        std::optional<Error> failure;
        std::size_t pendingOperations{};
        std::size_t retainedOperationResults{};
    };

    /**
     * @brief Owns one prepared playback lane, ordinary command transport, mixer and selected output.
     * @details Owner-thread tooling/control operations only. The host constructs this through the
     * non-installed composition contract; no device or backend is selected implicitly. Native
     * callbacks execute existing VoiceRenderRuntime and MixerGraphRuntime with bounded FIFO dispatch.
     * Close is asynchronous: the owner must continue Pump until Closed before releasing host
     * dependencies. Failed/unproved native detachment retains the complete ownership island under
     * ADR-062. Destroying an attached or incompletely retired owner terminates rather than leaking
     * a playing device or freeing unjoined callback memory (the existing StreamingService contract).
     * No authored source, editor widget or mutable scene pointer enters the callback.
     */
    class AudioFrontend final {
    public:
        /** @brief Destroys a closed or never-attached owner on control.
         * @pre Host has pumped Close to Closed; a never-started candidate may retire directly.
         */
        ~AudioFrontend();
        AudioFrontend(const AudioFrontend &) = delete;
        AudioFrontend &operator=(const AudioFrontend &) = delete;
        AudioFrontend(AudioFrontend &&) = delete;
        AudioFrontend &operator=(AudioFrontend &&) = delete;

        /** @brief Starts explicit selected-output acquisition.
         * @param deadline Host monotonic deadline, in the composition's clock domain.
         * @return Success or preserved startup failure; no implicit fallback.
         */
        [[nodiscard]] Result<void> Start(AudioMonotonicTimestamp deadline);
        /** @brief Polls one bounded output operation, drains facts and publishes queued work.
         * @param deadline Current host deadline for the next lifecycle operation.
         * @return Success or original output/stream error; cleanup remains queryable and retryable.
         */
        [[nodiscard]] Result<void> Pump(AudioMonotonicTimestamp deadline);
        /** @brief Admits one typed control for this lane's exact voice.
         * @param control Owned transport operation; foreign voices and inactive output fail.
         * @return Exact admission or explicit retry/rejection. Application occurs on the callback.
         */
        [[nodiscard]] Result<AudioCommandAdmission> Transport(AudioVoiceControlRequest control);
        /** @brief Copies and acknowledges completed operations on control, without allocation.
         * @param output Caller storage; at most 64 results are copied per call.
         * @return Number of results copied. Results remain retained on saturation; native resources
         * may retire independently. Applied means completed callback-block dispatch, not voice end.
         */
        [[nodiscard]] std::size_t DrainTransportResults(std::span<AudioFrontendOperationResult> output) noexcept;
        /** @brief Closes admission and requests ordered output teardown; repeated requests are harmless. */
        void Close() noexcept;
        /** @brief Returns owned control facts, never a native handle or mutable registry borrow. */
        [[nodiscard]] AudioFrontendSnapshot Snapshot() const;

    private:
        struct State;
        friend struct Internal::AudioFrontendComposition;
        /** @brief Publishes complete detached factory-owned state without activating output. */
        explicit AudioFrontend(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
