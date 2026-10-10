#pragma once

/** @file AudioFrontendState.h
 * @brief Target-private pinned callback storage and explicit output lifecycle facts.
 */

#include "Horo/Audio/Internal/AudioFrontendComposition.h"

#include <array>
#include <atomic>

namespace Horo::Audio {
    struct AudioFrontend::State final {
        enum class Operation : std::uint8_t {
            Open,
            Start,
            Quiesce,
            Stop,
            Close
        };
        std::optional<Internal::AudioFrontendResources> resources;
        std::optional<Internal::AudioFrontendOutput> output;
        AudioFrontendPhase phase{AudioFrontendPhase::Prepared};
        std::optional<Backend::OperationId> pending;
        Operation operation{Operation::Open};
        std::optional<Error> failure;
        bool closing{};
        bool opened{};
        bool started{};
        bool rendering{};
        bool ready{};
        bool quiesced{};
        bool detached{true};
        std::uint32_t maximumFrames{};
        AudioVoiceHandle voiceIdentity;
        std::atomic<const ErrorCodeDescriptor *> callbackFailure{};
        enum class OperationStatus : std::uint8_t {
            Pending,
            Applied,
            Rejected,
            Cancelled
        };
        static_assert(std::atomic<OperationStatus>::is_always_lock_free);

        /** @brief Sequence release publishes immutable slot input; terminal release publishes its error after last block use. */
        struct Receipt final {
            std::atomic<std::uint64_t> sequence{};
            std::atomic<OperationStatus> status{OperationStatus::Pending};
            const ErrorCodeDescriptor *error{};
        };

        std::array<Receipt, MaximumAudioFrontendOperations> receipts;

        /** @brief Fixed callback entry; resource owners and format are pinned before Start. */
        static Backend::RenderResult Process(void *context, const Backend::RenderInvocation &invocation) noexcept;
        /** @brief Executes bounded production command dispatch and mixing for an admitted rendering block. */
        Backend::RenderResult RenderBlock(const Backend::RenderInvocation &invocation) noexcept;
        /** @brief Reconciles output facts while preserving the first lifecycle failure. */
        Result<void> ReconcileOutput();
        /** @brief Advances startup and ordinary control publication while admission remains open. */
        Result<void> AdvanceActive(AudioMonotonicTimestamp deadline);
        /** @brief Validates the exact epoch on start/quiesce/stop terminal facts. */
        Result<void> ApplyPlaybackCompletion(const Backend::Completion &completion);
        /** @brief Reconciles remaining FIFO records and publishes cancellation only after native detachment. */
        Result<void> DrainCancelled();
        /** @brief Stages one asynchronous operation; rejection retains all ownership. */
        Result<void> Begin(Operation next, const Backend::Request &request, AudioMonotonicTimestamp deadline);
        /** @brief Reconciles an exact retained completion before acknowledging its backend slot. */
        Result<void> Poll();
        /** @brief Applies one completed output operation without acknowledging its backend slot. */
        Result<void> ApplyCompletion(const Backend::Completion &completion);
        /** @brief Preserves an output failure and its explicit callback/resource disposition. */
        Result<void> ApplyFailure(const Backend::Failed &failed);
        /** @brief Rejects an unexpected native outcome while preserving the entire ownership island. */
        Result<void> InvalidCompletion();
        /** @brief Validates negotiated output against both host intent and the prepared graph. */
        Result<void> AdmitOpened(const Backend::Opened &value);
        /** @brief Validates lifecycle facts and commits readiness only on matching epochs. */
        Result<void> Drain();
        /** @brief Validates and retains one backend fact independently of Start completion ordering. */
        Result<void> Observe(const Backend::Event &event);
        /** @brief Advances teardown only after preceding exact completion proofs. */
        Result<void> AdvanceClose(AudioMonotonicTimestamp deadline);
        /** @brief Releases callback owners, then stream jobs, only after native Stop proof. */
        Result<void> Release();
        /** @brief Publishes a callback result after the block's last borrowed-input use. */
        void CompleteOperation(std::uint64_t sequence, const ErrorCodeDescriptor *error) noexcept;
    };

    static_assert(std::atomic<const ErrorCodeDescriptor *>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

}  // namespace Horo::Audio
