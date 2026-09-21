#pragma once

/**
 * @file SequencePlayer.h
 * @brief Generation-fenced, allocation-free sequence playback state machine.
 */

#include "Horo/Cinematic/CinematicIdentity.h"
#include "Horo/Cinematic/CurveSampling.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>

namespace Horo::Cinematic {
    /** @brief Exact sequence timeline position shared with cooked curve sampling. */
    using SequenceTime = CurveTime;

    /** @brief Explicit owner-published lifecycle of one prepared sequence player. */
    enum class SequencePlaybackState : std::uint8_t {
        Ready,
        Playing,
        Paused,
        Stopping,
        Stopped,
        Closing,
        Failed,
        Count
    };

    /** @brief One observable signal produced by a successful player command. */
    enum class SequencePlaybackSignal : std::uint8_t {
        None,
        Started,
        Resumed,
        Paused,
        Advanced,
        StopRequested,
        Stopped,
        Seeked,
        RateChanged,
        Closing,
        Failed,
        Count
    };

    /** @brief Event-cursor action attached to a playback transition. */
    enum class SequenceEventTransitionPolicy : std::uint8_t {
        Unchanged,
        ResetWithoutDispatch,
        CloseAndDrainAdmitted,
        CloseAndDiscardPending,
        Count
    };

    /** @brief Exact bounded rational playback rate; zero is a valid rate and is not pause. */
    struct SequencePlaybackRate final {
        std::int32_t numerator{1};    /**< Signed rate numerator; negative rates play backward. */
        std::uint32_t denominator{1}; /**< Positive rate denominator. */
        constexpr auto operator<=>(const SequencePlaybackRate &) const noexcept = default;
    };

    /** @brief Generation-safe reference into one session-owned player registry. */
    struct SequencePlayerHandle final {
        CinematicRuntimeSessionId session; /**< Exact runtime-session generation. */
        SequencePlayerId player;           /**< Exact player generation within that session. */

        /** @brief Checks only the handle representation. @return True when both identities are valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return session.IsValid() && player.IsValid();
        }

        constexpr auto operator<=>(const SequencePlayerHandle &) const noexcept = default;
    };

    /** @brief Immutable observation snapshot published by the owning runtime service. */
    struct SequencePlayerSnapshot final {
        SequencePlayerHandle handle;
        SequencePlaybackState state{SequencePlaybackState::Ready};
        SequenceTime position{};
        SequenceTime duration{};
        SequencePlaybackRate rate{};
        std::uint64_t controlRevision{};
        constexpr auto operator<=>(const SequencePlayerSnapshot &) const noexcept = default;
    };

    /** @brief Fence attached to deferred work so late completions cannot mutate replacement state. */
    struct SequencePlayerOperationFence final {
        SequencePlayerHandle handle;
        std::uint64_t controlRevision{};
        constexpr auto operator<=>(const SequencePlayerOperationFence &) const noexcept = default;
    };

    /** @brief Complete result of one accepted state-machine command. */
    struct SequencePlayerTransition final {
        SequencePlaybackState previousState{SequencePlaybackState::Ready};
        SequencePlaybackState state{SequencePlaybackState::Ready};
        SequencePlaybackSignal signal{SequencePlaybackSignal::None};
        SequenceEventTransitionPolicy eventPolicy{SequenceEventTransitionPolicy::Unchanged};
        SequenceTime previousPosition{};
        SequenceTime position{};
        SequencePlaybackRate previousRate{};
        SequencePlaybackRate rate{};
        std::uint64_t controlRevision{};
        constexpr auto operator<=>(const SequencePlayerTransition &) const noexcept = default;
    };

    /** @brief Validated construction data supplied by the session-owned player registry. */
    struct SequencePlayerDescriptor final {
        SequencePlayerHandle handle;
        SequenceTime duration{};
        SequenceTime initialPosition{};
        SequencePlaybackRate initialRate{};
        constexpr auto operator<=>(const SequencePlayerDescriptor &) const noexcept = default;
    };

    /**
     * @brief Small explicit player state machine owned and mutated only by CinematicRuntimeService.
     * @note Successful commands perform constant bounded work and allocate no memory.
     * @note This type owns no asset, provider, callback, token, or native backend handle.
     */
    class SequencePlayer final {
    public:
        /**
         * @brief Validates one prepared player descriptor.
         * @param descriptor Session-issued identity, finite timeline bounds, and initial rate.
         * @return Ready player or a typed handle, time, or rate failure.
         */
        [[nodiscard]] static Result<SequencePlayer> Create(const SequencePlayerDescriptor &descriptor);

        /** @brief Returns the current immutable observation. @return Value snapshot of player state. */
        [[nodiscard]] SequencePlayerSnapshot Snapshot() const noexcept;

        /** @brief Captures the exact handle and revision for deferred work. @return Current operation fence. */
        [[nodiscard]] SequencePlayerOperationFence CaptureFence() const noexcept;

        /**
         * @brief Validates a deferred completion against the exact current player revision.
         * @param fence Previously captured operation fence.
         * @return Success or a typed invalid, unknown, or stale-handle failure.
         */
        [[nodiscard]] Result<void> ValidateFence(const SequencePlayerOperationFence &fence) const;

        /** @brief Starts or resumes playback. @param handle Exact current handle. @return Observable transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Play(const SequencePlayerHandle &handle);
        /** @brief Explicitly pauses playback without changing its rate. @param handle Exact current handle. @return Observable transition.
         */
        [[nodiscard]] Result<SequencePlayerTransition> Pause(const SequencePlayerHandle &handle);
        /**
         * @brief Closes future evaluation/event admission and begins orderly draining.
         * @param handle Exact current handle.
         * @return Stop-request transition; already admitted events remain drainable.
         */
        [[nodiscard]] Result<SequencePlayerTransition> Stop(const SequencePlayerHandle &handle);
        /** @brief Publishes Stopped after admitted stop work retires. @param handle Exact current handle. @return Terminal transition. */
        [[nodiscard]] Result<SequencePlayerTransition> FinishStop(const SequencePlayerHandle &handle);
        /**
         * @brief Publishes an arbitrary target position without traversing from the old cursor.
         * @param handle Exact current handle.
         * @param target Direct random-access timeline target in [0, duration].
         * @return Atomic seek transition that suppresses skipped event dispatch.
         */
        [[nodiscard]] Result<SequencePlayerTransition> Seek(const SequencePlayerHandle &handle, SequenceTime target);
        /**
         * @brief Commits one successfully evaluated position without changing the control revision.
         * @param fence Exact control fence captured before the owner-boundary evaluation.
         * @param position Position produced by the all-or-none evaluation attempt.
         * @return Observable advancement or a typed stale, state, or time failure.
         * @note Only the session owner may publish evaluated time; commands still advance the control revision.
         */
        [[nodiscard]] Result<SequencePlayerTransition> CommitEvaluationPosition(const SequencePlayerOperationFence &fence,
                                                                                SequenceTime position);
        /**
         * @brief Changes the exact playback rate without changing pause state.
         * @param handle Exact current handle.
         * @param rate Bounded rational rate; zero freezes advancement but remains distinct from Paused.
         * @return Observable rate transition.
         */
        [[nodiscard]] Result<SequencePlayerTransition> SetPlaybackSpeed(const SequencePlayerHandle &handle, SequencePlaybackRate rate);
        /**
         * @brief Begins immediate owner disposal for cancellation, scene loss, or shutdown.
         * @param handle Exact current handle.
         * @return Closing transition that discards pending, not-yet-admitted work.
         */
        [[nodiscard]] Result<SequencePlayerTransition> Close(const SequencePlayerHandle &handle);
        /** @brief Publishes Stopped after closing resources retire. @param handle Exact current handle. @return Terminal transition. */
        [[nodiscard]] Result<SequencePlayerTransition> FinishClose(const SequencePlayerHandle &handle);
        /** @brief Publishes one sticky terminal failure. @param handle Exact current handle. @return Failed transition. */
        [[nodiscard]] Result<SequencePlayerTransition> Fail(const SequencePlayerHandle &handle);

    private:
        explicit SequencePlayer(const SequencePlayerDescriptor &descriptor) noexcept;

        [[nodiscard]] Result<void> ValidateHandle(const SequencePlayerHandle &handle) const;
        [[nodiscard]] Result<void> ValidateControllableHandle(const SequencePlayerHandle &handle) const;
        [[nodiscard]] Result<SequencePlayerTransition> Change(const SequencePlayerHandle &handle, SequencePlaybackState state,
                                                              SequencePlaybackSignal signal, SequenceEventTransitionPolicy eventPolicy);
        [[nodiscard]] Result<SequencePlayerTransition> ApplyValueChange(const SequencePlayerHandle &handle,
                                                                        const SequencePlayerSnapshot &requested,
                                                                        SequencePlaybackSignal signal,
                                                                        SequenceEventTransitionPolicy eventPolicy);
        [[nodiscard]] SequencePlayerTransition NoChange() const noexcept;
        [[nodiscard]] SequencePlayerTransition TransitionFrom(const SequencePlayerSnapshot &previous, SequencePlaybackSignal signal,
                                                              SequenceEventTransitionPolicy eventPolicy) const noexcept;

        SequencePlayerSnapshot snapshot_;
    };
}  // namespace Horo::Cinematic
