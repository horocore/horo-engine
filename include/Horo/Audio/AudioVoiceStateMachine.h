#pragma once

/**
 * @file AudioVoiceStateMachine.h
 * @brief Backend-neutral generation-checked voice admission and lifecycle contract.
 */

#include "Horo/Audio/AudioIdentity.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace Horo::Audio {
    /** @brief Maximum number of voice slots admitted by one prepared state machine. */
    inline constexpr std::uint32_t MaximumAudioVoiceSlots = MaximumAudioHandleSlots;

    /** @brief Closed lifecycle states for one admitted playback voice. */
    enum class AudioVoiceState : std::uint8_t {
        Created,
        Ready,
        Scheduled,
        Playing,
        Paused,
        Virtual,
        Stopping,
        Stopped,
        Finished,
        Cancelled,
        Failed,
    };

    /** @brief Exact terminal disposition represented by a terminal voice state. */
    enum class AudioVoiceTerminalReason : std::uint8_t {
        Finished,
        Stopped,
        Cancelled,
        Failed,
    };

    /**
     * @brief Reports whether a voice state is terminal.
     * @param state State to inspect.
     * @return True only for Stopped, Finished, Cancelled or Failed.
     */
    [[nodiscard]] bool IsTerminalAudioVoiceState(AudioVoiceState state) noexcept;

    /**
     * @brief Maps a terminal voice state to its retained terminal reason.
     * @param state State to inspect.
     * @return Terminal reason for a terminal state, or no value for an active state.
     */
    [[nodiscard]] std::optional<AudioVoiceTerminalReason> AudioVoiceTerminalReasonForState(AudioVoiceState state) noexcept;

    /**
     * @brief Fixed admission dimensions for one voice state machine.
     *
     * All slot and state storage is reserved by Create. The owner identifies the
     * exact process-local audio runtime generation and is copied into every issued
     * voice handle.
     */
    struct AudioVoiceStateMachineConfig final {
        AudioRuntimeId owner;                                                       /**< Exact process-local audio runtime generation. */
        std::uint32_t maximumVoices{64};                                            /**< Fixed number of one-based voice slots. */
        std::uint32_t maximumGeneration{std::numeric_limits<std::uint32_t>::max()}; /**< Non-wrapping slot ceiling. */
    };

    /**
     * @brief Immutable state projection for one currently resolvable voice handle.
     *
     * The optional terminal reason is present only for Stopped, Finished, Cancelled or
     * Failed. A snapshot is a value copy and retains no slot lifetime.
     */
    struct AudioVoiceSnapshot final {
        AudioVoiceHandle voice;                                 /**< Exact owner, slot and generation queried. */
        AudioVoiceState state{AudioVoiceState::Created};        /**< Current lifecycle state. */
        std::optional<AudioVoiceTerminalReason> terminalReason; /**< Present only for a terminal state. */

        [[nodiscard]] constexpr auto operator<=>(const AudioVoiceSnapshot &) const noexcept = default;
    };

    /**
     * @brief Owns a fixed-capacity generation-checked voice registry and its lifecycle states.
     *
     * This value is a control-owner primitive. Create performs all storage allocation;
     * successful steady-state calls do not grow or allocate storage. Handles are
     * process-local and cannot be serialized or used across runtime owners. Release
     * is admitted only after a voice has reached exactly one terminal state.
     */
    class AudioVoiceStateMachine final {
    public:
        /**
         * @brief Reserves the complete voice slot and state storage.
         * @param config Valid runtime owner, positive voice capacity and non-wrapping generation ceiling.
         * @return Prepared state machine or a typed identity, capacity or allocation failure.
         */
        [[nodiscard]] static Result<AudioVoiceStateMachine> Create(const AudioVoiceStateMachineConfig &config);

        /** @brief Destroys the prepared registry and its private fixed-capacity storage. */
        ~AudioVoiceStateMachine();

        AudioVoiceStateMachine(const AudioVoiceStateMachine &) = delete;
        AudioVoiceStateMachine &operator=(const AudioVoiceStateMachine &) = delete;
        /** @brief Transfers sole ownership of the prepared registry. @param other Source owner. */
        AudioVoiceStateMachine(AudioVoiceStateMachine &&other) noexcept;
        /** @brief Replaces this owner with another prepared registry. @param other Source owner. @return This owner. */
        AudioVoiceStateMachine &operator=(AudioVoiceStateMachine &&other) noexcept;

        /**
         * @brief Admits one new voice in Created state.
         * @return A generation-checked handle, or VoiceAdmissionClosed, capacity, generation or runtime failure.
         */
        [[nodiscard]] Result<AudioVoiceHandle> CreateVoice();

        /**
         * @brief Reads the current state of one exact live voice generation.
         * @param voice Owner-issued voice handle.
         * @return Current state, or a typed malformed, foreign-owner or stale-handle failure.
         */
        [[nodiscard]] Result<AudioVoiceState> State(const AudioVoiceHandle &voice) const;

        /**
         * @brief Copies one exact live voice state and terminal projection.
         * @param voice Owner-issued voice handle.
         * @return Immutable snapshot, or a typed malformed, foreign-owner or stale-handle failure.
         */
        [[nodiscard]] Result<AudioVoiceSnapshot> Snapshot(const AudioVoiceHandle &voice) const;

        /**
         * @brief Applies one legal non-cancellation state transition.
         * @param voice Owner-issued voice handle.
         * @param state Requested successor state.
         * @return Success, or VoiceInvalidTransition, VoiceAdmissionClosed or handle validation failure.
         * @post Failure leaves the voice state unchanged.
         */
        [[nodiscard]] Result<void> Transition(const AudioVoiceHandle &voice, AudioVoiceState state) const;

        /**
         * @brief Moves one non-terminal voice directly to the Cancelled terminal state.
         * @param voice Owner-issued voice handle.
         * @return Success, or VoiceInvalidTransition, VoiceAdmissionClosed or handle validation failure.
         * @post A successful call publishes exactly one terminal state and never reopens the voice.
         */
        [[nodiscard]] Result<void> Cancel(const AudioVoiceHandle &voice) const;

        /**
         * @brief Releases one terminal voice slot for generation-safe reuse.
         * @param voice Exact terminal voice handle.
         * @return Success, or a typed handle or lifecycle failure.
         * @post Reuse advances the slot generation; the released handle remains stale forever.
         */
        [[nodiscard]] Result<void> Release(const AudioVoiceHandle &voice);

        /**
         * @brief Closes admission and cancels every currently non-terminal voice.
         * @return Success; repeated shutdown calls are idempotent.
         * @post CreateVoice is rejected and every admitted live voice is terminal.
         */
        [[nodiscard]] Result<void> BeginShutdown();

    private:
        struct Implementation;

        /** @brief Resolves one exact live voice handle or returns its typed validation failure. */
        [[nodiscard]] Result<std::uint32_t> ResolveSlot(const AudioVoiceHandle &voice) const;

        /** @brief Adopts fully prepared private storage. */
        explicit AudioVoiceStateMachine(std::unique_ptr<Implementation> implementation) noexcept;

        std::unique_ptr<Implementation> implementation_;
    };
}  // namespace Horo::Audio
