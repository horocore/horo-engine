#pragma once

/**
 * @file AudioEventQueue.h
 * @brief Bounded callback-producer to control-consumer audio outcome transport.
 */

#include "Horo/Audio/AudioCallbackEvents.h"
#include "Horo/Audio/AudioCommands.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace Horo::Audio {
    inline constexpr std::uint32_t MaximumAudioEventSlots = 65'536;

    /** @brief Final disposition of one admitted voice operation. */
    enum class AudioVoiceTerminalReason : std::uint8_t {
        Finished,
        Stopped,
        Cancelled,
        Failed,
    };

    /** @brief Exactly one terminal outcome for an accepted voice operation. */
    struct AudioVoiceTerminalEvent final {
        AudioCommandScope scope;           /**< Exact runtime epoch and scene context. */
        std::uint64_t acceptedSequence{};  /**< Accepted command sequence correlated by control. */
        AudioVoiceHandle voice;            /**< Exact voice generation that reached terminal state. */
        AudioVoiceTerminalReason reason{}; /**< Typed terminal disposition. */
    };

    /** @brief Callback evidence that one accepted logical release crossed its last-use boundary. */
    struct AudioResourceReleaseEvent final {
        AudioCommandScope scope;          /**< Exact runtime epoch and scene context. */
        std::uint64_t acceptedSequence{}; /**< Accepted release command sequence. */
        AudioMemoryHandle storage;        /**< Exact prepared storage generation no longer used by this callback. */
        std::uint64_t completedEpoch{};   /**< Non-zero callback completion watermark; queue consumption alone cannot reclaim. */
    };

    /** @brief Terminal callback outcome correlated to one accepted operation token. */
    using AudioTerminalEvent = std::variant<AudioVoiceTerminalEvent, AudioResourceReleaseEvent>;

    /** @brief Owned fixed-size event consumed by the control runtime. */
    using AudioControlEvent = std::variant<AudioVoiceTerminalEvent, AudioResourceReleaseEvent, AudioCallbackEvent>;

    /** @brief One queue record; FIFO position is transport state and not a lifecycle authority. */
    struct AudioControlEventRecord final {
        AudioControlEvent event;
    };

    /** @brief Fixed-size publication result; rejected critical work remains caller-owned. */
    enum class AudioEventPublishStatus : std::uint8_t {
        Published,
        TelemetryDropped,
        CriticalRetry,
        DuplicateTerminal,
        InvalidEvent,
        Closed,
        Inactive,
    };

    /** @brief State of one move-only callback-owned terminal publication right. */
    enum class AudioCompletionTokenState : std::uint8_t {
        Pending,
        Published,
        Inactive,
    };

    /**
     * @brief Move-only right to publish at most one terminal result for an accepted operation.
     * @details Control creates the token after command admission, then transfers exclusive ownership to callback state.
     *          Queue saturation leaves it Pending for retry; successful publication consumes the right permanently.
     */
    class AudioCompletionToken final {
    public:
        /**
         * @brief Creates one pending terminal publication right.
         * @param scope Exact admitted runtime epoch and scene context.
         * @param acceptedSequence Non-zero accepted command sequence.
         * @return Pending token or AudioErrors::EventQueueInvalid.
         */
        [[nodiscard]] static Result<AudioCompletionToken> Create(AudioCommandScope scope, std::uint64_t acceptedSequence);

        /** @brief Transfers exclusive publication ownership and leaves @p other inactive. */
        AudioCompletionToken(AudioCompletionToken &&other) noexcept;
        AudioCompletionToken &operator=(AudioCompletionToken &&) = delete;
        AudioCompletionToken(const AudioCompletionToken &) = delete;
        AudioCompletionToken &operator=(const AudioCompletionToken &) = delete;

        /** @brief Returns the exact admitted scope. @return Owned immutable scope. */
        [[nodiscard]] const AudioCommandScope &Scope() const noexcept;
        /** @brief Returns the accepted command sequence. @return Non-zero while pending or published. */
        [[nodiscard]] std::uint64_t AcceptedSequence() const noexcept;
        /** @brief Returns Pending, Published or Inactive. @return Current exclusive-right state. */
        [[nodiscard]] AudioCompletionTokenState State() const noexcept;

    private:
        /** @brief Constructs a validated pending right. */
        AudioCompletionToken(AudioCommandScope scope, std::uint64_t acceptedSequence) noexcept;
        /** @brief Consumes this right after its exact terminal record is published. */
        void MarkPublished() noexcept;

        AudioCommandScope scope_;
        std::uint64_t acceptedSequence_{};
        AudioCompletionTokenState state_{AudioCompletionTokenState::Inactive};

        friend class AudioEventQueue;
    };

    /** @brief Fixed-size observable pressure counters; every counter saturates instead of wrapping. */
    struct AudioEventQueueStats final {
        std::uint32_t depth{};              /**< Approximate live SPSC occupancy at the control sampling point. */
        std::uint32_t terminalEvents{};     /**< Successfully published terminal operation results. */
        std::uint32_t deviceEvents{};       /**< Successfully published callback/device facts. */
        std::uint32_t droppedTelemetry{};   /**< Best-effort underrun records dropped at ordinary capacity. */
        std::uint32_t criticalRetries{};    /**< Critical records retained by caller because the ring was full. */
        std::uint32_t duplicateTerminals{}; /**< Repeated attempts using an already consumed completion token. */
    };

    /** @brief Preparation-only dimensions and exact producer generation. */
    struct AudioEventQueueDescriptor final {
        AudioRuntimeId owner;              /**< Exact audio runtime generation. */
        AudioMemoryPoolId storageIdentity; /**< Host-unique EventStorage pool identity. */
        std::uint64_t commandEpoch{};      /**< Exact command/runtime epoch for terminal outcomes. */
        AudioDeviceEpoch callbackEpoch;    /**< Exact device/format/callback generation for device facts. */
        std::uint64_t clockDomain{};       /**< Non-zero mapped Horo monotonic clock domain. */
        std::uint32_t slots{};             /**< Power of two in [2, MaximumAudioEventSlots]. */
        std::uint32_t criticalSlots{};     /**< Positive reserve smaller than slots. */
        std::size_t budgetBytes{};         /**< Event records and pool metadata; fixed owner/cursors excluded. */
    };

    /**
     * @brief Preallocated SPSC queue with one callback producer and one control consumer.
     * @details Terminal outcomes and lifecycle-critical callback facts may use the complete ring. Best-effort
     *          underrun telemetry cannot consume the critical reserve and increments observable drop accounting.
     *          Device-worker producers must funnel through the callback owner or use a separately composed ingress.
     */
    class AudioEventQueue final {
    public:
        /**
         * @brief Prepares all queue storage outside the callback.
         * @param descriptor Valid runtime, command, device, clock, capacity and EventStorage facts.
         * @return Prepared queue or a typed event-queue, memory-budget or allocation failure.
         */
        [[nodiscard]] static Result<AudioEventQueue> Create(const AudioEventQueueDescriptor &descriptor);
        /** @brief Destroys quiescent storage only after producer detachment and control drain. */
        ~AudioEventQueue();
        /** @brief Transfers a quiescent queue owner and leaves @p other inert. */
        AudioEventQueue(AudioEventQueue &&other) noexcept;
        /** @brief Replaces a quiescent queue owner and leaves @p other inert. @return This queue. */
        AudioEventQueue &operator=(AudioEventQueue &&other) noexcept;
        AudioEventQueue(const AudioEventQueue &) = delete;
        AudioEventQueue &operator=(const AudioEventQueue &) = delete;

        /**
         * @brief Callback-only publication of one exact operation terminal.
         * @param token Exclusive pending right created after operation admission; consumed only on success.
         * @param event Exact matching voice or resource-release outcome.
         * @return Published, retry, duplicate, invalid, closed or inactive without allocation, locking or callbacks.
         */
        [[nodiscard]] AudioEventPublishStatus TryPublishTerminal(AudioCompletionToken &token, const AudioTerminalEvent &event) noexcept;

        /**
         * @brief Callback-only publication of a validated device/callback fact.
         * @param event Fact for the descriptor's exact callback epoch and clock domain.
         * @return Published; observable telemetry drop; or a retained critical retry/validation/lifecycle status.
         */
        [[nodiscard]] AudioEventPublishStatus TryPublishDevice(const AudioCallbackEvent &event) noexcept;

        /**
         * @brief Control-only acquire/copy of at most one record.
         * @param record Output assigned only when an event is consumed.
         * @return True for one owned record; false leaves @p record unchanged.
         */
        [[nodiscard]] bool TryConsume(AudioControlEventRecord &record) noexcept;

        /** @brief Callback-producer closes publication after its final record; accepted records remain consumable. */
        void Close() noexcept;
        /** @brief Returns pressure accounting visible to control. @return Atomic bounded counter snapshot. */
        [[nodiscard]] AudioEventQueueStats Stats() const noexcept;
        /** @brief Checks closed-and-empty state; it does not prove native callback detachment or resource reclamation. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct State;
        /** @brief Publishes fully prepared storage. */
        explicit AudioEventQueue(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
