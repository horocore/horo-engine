#pragma once

#include "Horo/Animation/AnimationGraphInstance.h"

#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Animation::Detail {
    /** @brief Stable authoring identity retained independently of dense runtime indexes. */
    struct StateMachineStateId final {
        std::uint64_t value{};
        auto operator<=>(const StateMachineStateId &) const = default;
    };

    /** @brief Stable transition identity used to break equal-priority ties. */
    struct StateMachineTransitionId final {
        std::uint64_t value{};
        auto operator<=>(const StateMachineTransitionId &) const = default;
    };

    /** @brief Exact compiled state access; the graph owns its clip/subgraph execution separately. */
    struct StateMachineState final {
        StateMachineStateId id;
    };

    /** @brief Scalar comparisons; float release thresholds give persistent hysteresis. */
    using StateMachinePredicate = GraphStatePredicate;

    /** @brief A compiler-resolved parameter access and finite typed threshold. */
    struct StateMachineCondition final {
        std::uint32_t parameter{};
        StateMachinePredicate predicate{};
        GraphParameterValue threshold{0.0F};
        float releaseThreshold{}; /**< Above releases at <= this value; Below at >= this value. */
    };

    /** @brief Which active semantic state may supply a higher-priority interrupting transition. */
    using StateMachineInterruption = GraphStateInterruption;
    /** @brief Finite transition-weight vocabulary; joint composition belongs to the pose kernel. */
    using StateMachineBlendCurve = GraphStateBlendCurve;

    /** @brief Immutable compiler-resolved transition; condition ranges are disjoint and complete. */
    struct StateMachineTransition final {
        StateMachineTransitionId id;
        std::uint32_t source{};
        std::uint32_t destination{};
        std::int32_t priority{}; /**< Larger values win; equal values use the lower stable transition ID. */
        std::uint32_t firstCondition{};
        std::uint32_t conditions{};
        AnimationDeltaTime duration{};
        std::optional<AnimationTime> exitTime{}; /**< Nonnegative elapsed player time, including completed loops. */
        StateMachineInterruption interruption{StateMachineInterruption::None};
        StateMachineBlendCurve curve{StateMachineBlendCurve::Linear};
    };

    /** @brief Invocation-only immutable views; the compiled graph retains their actual owning storage. */
    struct StateMachinePlan final {
        static constexpr std::uint32_t MaximumStates = 256;
        static constexpr std::uint32_t MaximumTransitions = 2048;
        static constexpr std::uint32_t MaximumConditions = 4096;
        static constexpr std::uint32_t MaximumLoopEvents = 256;
        std::span<const StateMachineState> states;
        std::span<const StateMachineTransition> transitions;
        std::span<const StateMachineCondition> conditions;
        std::uint32_t initial{};
    };

    /** @brief Distinct state-entry incarnation; even a self-transition retires the earlier entry. */
    struct StateMachineActivation final {
        std::uint32_t state{};
        std::uint64_t generation{};
        auto operator<=>(const StateMachineActivation &) const = default;
    };

    /** @brief Committed or staged control state; all mutable storage is owned by the graph instance. */
    struct StateMachineRuntime final {
        StateMachineActivation source{};
        std::optional<StateMachineActivation> destination{};
        std::optional<std::uint32_t> transition{};
        AnimationDeltaTime elapsed{};
        std::uint64_t nextActivation{1};
        bool initialized{};
        bool frozenSource{}; /**< Interruption asks the pose owner to retain the current composed pose. */
        bool operator==(const StateMachineRuntime &) const = default;
    };

    /** @brief Actual player traversal evidence for one exact active state-entry incarnation. */
    struct StateMachinePlayback final {
        StateMachineActivation activation;
        AnimationTime elapsed{}; /**< Exact elapsed authored player time supplied by its sole cursor owner. */
        std::uint32_t loops{};
        std::uint64_t firstLoopOrdinal{}; /**< Non-reused traversal ordinal, also when traversing in reverse. */
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
    };

    /** @brief Ordered staged lifecycle vocabulary; none of these records invokes a consumer callback. */
    using StateMachineEventKind = GraphStateEventKind;

    /** @brief One staged record; the graph adds exact instance/occurrence/tick and event ordinal at publication. */
    struct StateMachineEvent final {
        StateMachineEventKind kind{};
        StateMachineStateId state{};
        StateMachineTransitionId transition{};
        std::uint64_t activation{};
        std::uint64_t loopOrdinal{};
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
        bool operator==(const StateMachineEvent &) const = default;
    };

    /** @brief Complete candidate inputs, borrowed only for this synchronous bounded invocation. */
    struct StateMachineStepInput final {
        std::span<const GraphParameterValue> parameters;
        std::span<const StateMachinePlayback>
            playback;                 /**< Source then destination; optional exact initial-entry observation before initialization. */
        AnimationTimeDelta advance{}; /**< Nonnegative admitted local transition-clock advance; zero holds blend time. */
        CancellationToken cancellation{};
    };

    /** @brief Typed staging result; event bytes are valid only on success and never already published. */
    struct StateMachineStepOutcome final {
        GraphInstanceFailure failure{GraphInstanceFailure::None};
        std::uint32_t events{};
        float weight{};
        bool captureSource{};    /**< Capture the pre-interruption composed pose into admitted owner storage. */
        bool resetDestination{}; /**< Start the new state's clip/subgraph occurrence at its explicit entry boundary. */
    };

    /** @brief Checks the common immutable table ceilings before either preparation or tick evidence admission.
     * @param plan Candidate tables, without dereferencing their entries.
     * @return Whether all four caller-independent table-size invariants hold.
     */
    [[nodiscard]] bool StateMachineStorageAdmitted(const StateMachinePlan &plan) noexcept;

    /** @brief Validates detached immutable plan data at compiler/control time without allocation.
     * @param plan Complete candidate tables.
     * @param parameters Exact canonical parameter declarations used to compile condition accesses.
     * @return Finite typed failure or None; callers retain only successfully validated immutable tables.
     */
    [[nodiscard]] GraphInstanceFailure ValidateStateMachine(const StateMachinePlan &plan,
                                                            std::span<const GraphParameter> parameters) noexcept;

    /** @brief Computes candidate latches and stable priority selection without allocation or pose sampling.
     * @param plan Previously admitted immutable tables.
     * @param runtime Staged semantic state used for interruption admission.
     * @param input Exact copied parameter block and player evidence.
     * @param latches Complete candidate hysteresis storage; modified only within the private candidate.
     * @return Selected transition index, or no eligible transition.
     */
    [[nodiscard]] std::optional<std::uint32_t> SelectStateMachineTransition(const StateMachinePlan &plan,
                                                                            const StateMachineRuntime &runtime,
                                                                            const StateMachineStepInput &input,
                                                                            std::span<std::uint8_t> latches) noexcept;

    /** @brief Stages at most one new transition and complete bounded lifecycle records for an attempted tick.
     * @param plan Previously admitted immutable tables; actual graph owns storage throughout invocation.
     * @param committed Last committed runtime state; never changed.
     * @param committedLatches Complete last committed hysteresis block.
     * @param input Attempted tick parameters, exact active player observations, local clock and cancellation.
     * @param candidate Private next-state buffer, discarded on failure/Abort.
     * @param candidateLatches Complete private latch storage, distinct from committedLatches.
     * @param events Caller-owned complete candidate event storage, discarded on failure/Abort.
     * @return Typed outcome, finite weight and event count; failure returns zero published/staged-valid events.
     * @details This kernel owns no cursor, pose, asset borrow, callback, heap storage or global state.
     *           Candidate bytes may change on failure; the graph must commit them only after all tick work succeeds.
     */
    [[nodiscard]] StateMachineStepOutcome StepStateMachine(const StateMachinePlan &plan, const StateMachineRuntime &committed,
                                                           std::span<const std::uint8_t> committedLatches,
                                                           const StateMachineStepInput &input, StateMachineRuntime &candidate,
                                                           std::span<std::uint8_t> candidateLatches,
                                                           std::span<StateMachineEvent> events) noexcept;
}  // namespace Horo::Animation::Detail
