#pragma once

/** @file AnimationGraphGameplay.h
 * @brief Portable graph commands and committed observations for gameplay consumers.
 */

#include "Horo/Animation/AnimationClip.h"
#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Foundation/CancellationToken.h"

namespace Horo::Animation {
    /** @brief Committed disposition of a Graph-owned motion request. */
    enum class AnimationRootMotionDisposition : std::uint8_t {
        Unavailable,
        Rejected,
        Consumed
    };

    /** @brief Exact typed parameter snapshot entry, identified independently of
     * declaration order. */
    struct GraphParameterInput final {
        GraphParameterId id{};
        GraphParameterValue value{0.0F};
    };

    /** @brief Committed state lifecycle records; clip notifies remain their
     * separate Animation responsibility. */
    enum class GraphStateEventKind : std::uint8_t {
        Entered,
        Exited,
        TransitionStarted,
        TransitionInterrupted,
        TransitionCompleted,
        Loop
    };

    /** @brief Copied immutable lifecycle occurrence, fenced by instance, compiled
     * occurrence, tick and entry generation. */
    struct GraphStateEvent final {
        AnimationInstanceHandle instance{};
        AnimationTickId tick{};
        GraphSourceLocation source{};
        std::uint32_t occurrence{};
        std::uint32_t ordinal{};
        GraphStateEventKind kind{};
        GraphStateId state{};
        GraphTransitionId transition{};
        std::uint64_t activation{};
        std::uint64_t loopOrdinal{};
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
        bool operator==(const GraphStateEvent &) const = default;
    };

    /** @brief Exact instance-generation scoped request identity; copying it does
     * not create or retain a request. */
    struct GraphActionHandle final {
        AnimationInstanceHandle instance{};
        std::uint64_t ordinal{};

        [[nodiscard]] bool IsValid() const noexcept {
            return instance.IsValid() && ordinal != 0;
        }

        bool operator==(const GraphActionHandle &) const = default;
    };
    /** @brief Explicit command admission policy; accepted commands still obey
     * active interruption policy at Commit. */
    enum class GraphActionQueuePolicy : std::uint8_t {
        IfIdle,
        Queue,
        Interrupt,
        Count
    };
    /** @brief Explicit owner cancellation handoff. */
    enum class GraphActionStopPolicy : std::uint8_t {
        BlendOut,
        Cut,
        Count
    };
    /** @brief Action lifecycle is separate from clip notifies and state-machine
     * transition events. */
    enum class GraphActionPhase : std::uint8_t {
        Pending,
        Queued,
        Playing,
        BlendingOut,
        Terminal
    };
    /** @brief Committed terminal result, except Shutdown which is an explicit owner
     * terminal operation. */
    enum class GraphActionCompletionReason : std::uint8_t {
        None,
        Completed,
        Cancelled,
        Interrupted,
        Rejected,
        Shutdown
    };
    /** @brief Finite lightweight command failure without owning diagnostic
     * allocation. */
    enum class GraphActionFailure : std::uint8_t {
        None,
        Closed,
        ThreadViolation,
        EvaluationPending,
        Malformed,
        BindingMismatch,
        CapacityExceeded,
        HandleStale,
        NotTerminal
    };

    /** @brief One immutable command intent, copied into the existing owner request
     * table. */
    struct GraphActionRequest final {
        GraphActionId action{};
        GraphActionSectionId startSection{};
        GraphActionQueuePolicy queue{GraphActionQueuePolicy::Queue};
        CancellationToken cancellation{};
    };

    /** @brief Complete retained request fact; terminal records live until explicit
     * owner release. */
    struct GraphActionObservation final {
        GraphActionHandle handle{};
        GraphActionId action{};
        GraphActionSlotId slot{};
        GraphActionPhase phase{GraphActionPhase::Pending};
        GraphActionCompletionReason reason{GraphActionCompletionReason::None};
        GraphActionSectionId section{};
        AnimationTime sampleTime{};
        float weight{};
        AnimationTickId terminalTick{};
        bool operator==(const GraphActionObservation &) const = default;
    };

    /** @brief Typed no-allocation command response; handle is valid only after
     * actual request admission. */
    struct GraphActionCommandOutcome final {
        GraphActionFailure failure{GraphActionFailure::None};
        GraphActionHandle handle{};

        [[nodiscard]] bool HasValue() const noexcept {
            return failure == GraphActionFailure::None;
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }
    };

    /** @brief Typed owner query of one exact live/retained terminal request. */
    struct GraphActionQueryOutcome final {
        GraphActionFailure failure{GraphActionFailure::None};
        GraphActionObservation observation{};

        [[nodiscard]] bool HasValue() const noexcept {
            return failure == GraphActionFailure::None;
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }
    };

    /** @brief Actual point/window delivery phase, separate from graph state/action lifecycle records. */
    enum class AnimationNotifyPhase : std::uint8_t {
        End,
        Begin,
        Point
    };
    /** @brief Truthful cause of delivery/closure, including discontinuities with no fabricated endpoint travel. */
    enum class AnimationNotifyReason : std::uint8_t {
        Crossed,
        Activation,
        Resumed,
        Loop,
        SectionChanged,
        Cancelled,
        Interrupted,
        Inactive,
        Suppressed,
        ReverseIneligible,
        PlaybackEnd,
        Reentered
    };

    /** @brief Stable value identity for the exact instance/player/revision/tick/directed occurrence. */
    struct AnimationEventOccurrenceId final {
        AnimationInstanceHandle instance{};
        AnimationTickId tick{};
        AnimationClipId clip{};
        AnimationClipGeneration clipGeneration{};
        AnimationEventMarkerId marker{};
        std::uint32_t player{};
        std::uint64_t serial{};
        std::int64_t loopOrdinal{};
        AnimationNotifyPhase phase{AnimationNotifyPhase::Point};
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
        bool operator==(const AnimationEventOccurrenceId &) const = default;
    };

    /** @brief Fully owned fixed-capacity occurrence; payload remains valid after clip reload and instance retirement. */
    struct AnimationEventOccurrence final {
        AnimationEventOccurrenceId id{};
        AnimationEventTypeId type{};
        std::uint32_t schemaRevision{};
        AnimationEventPayload payload{};
        AnimationTime time{};
        GraphActionHandle action{};
        std::uint64_t windowBeginSerial{}; /**< End refers to the actual previously delivered Begin; zero only for points. */
        AnimationNotifyReason reason{AnimationNotifyReason::Crossed};
        bool operator==(const AnimationEventOccurrence &) const = default;
    };
}  // namespace Horo::Animation
