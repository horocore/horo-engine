#pragma once

/** @file StateMachine.h
 * @brief Typed hierarchical state assets and bounded owner-thread execution.
 */

#include "Horo/AI/AITaskLifecycle.h"
#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/DecisionAssetValidation.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace StateMachineErrors {
        /** @brief Missing, duplicated or inconsistent state-machine topology or initial-child selection. */
        extern const ErrorCodeDescriptor TopologyInvalid;
        /** @brief Parent hierarchy contains a cycle or exceeds the fixed depth bound. */
        extern const ErrorCodeDescriptor HierarchyInvalid;
        /** @brief A step is repeated, malformed, oversized, reentrant or no longer admitted by the task lifecycle. */
        extern const ErrorCodeDescriptor StepInvalid;
    }  // namespace StateMachineErrors
    struct StateMachineEventIdentityTag;
    /** @brief Stable gameplay event identity; event payload facts enter the shared blackboard at BlackboardSync. */
    using StateMachineEventId = AiStableIdentity<StateMachineEventIdentityTag>;

    /** @brief Fixed activation and evaluation ceilings, independent of graphics or host speed. */
    struct StateMachineLimits final {
        static constexpr std::size_t States = 256;
        static constexpr std::size_t Transitions = 512;
        static constexpr std::size_t Depth = 16;
        static constexpr std::size_t GuardsPerTransition = 8;
        static constexpr std::size_t EventsPerStep = 32;
        static constexpr std::size_t TransitionsPerStep = 16;
    };

    /** @brief Closed guard operators; ordered comparisons accept only integer or scalar keys. */
    enum class StateMachineGuardOperator : std::uint8_t {
        Equal,
        NotEqual,
        Less,
        Greater,
        Present,
        Count
    };

    /** @brief One conjunctive guard over a compiler-admitted typed blackboard key. */
    struct StateMachineGuard final {
        BlackboardKeyId key; /**< Must occur in the owning transition node's shared plan bindings. */
        StateMachineGuardOperator operation{StateMachineGuardOperator::Equal};
        BlackboardScalarValue operand{false}; /**< Ignored by Present; otherwise must match the admitted scalar kind. */
    };

    /** @brief One semantic state; hierarchy and actions use stable identities, never display labels. */
    struct StateMachineState final {
        DecisionNodeId id;
        DecisionNodeId parent;       /**< Invalid for top-level states. */
        DecisionNodeId initialChild; /**< Required exactly when this state has children. */
        std::optional<TaskId> entry;
        std::optional<TaskId> update;
        std::optional<TaskId> exit;
    };

    /** @brief One external transition; self transitions exit and re-enter the source state. */
    struct StateMachineTransition final {
        DecisionNodeId id;     /**< Stable transition identity, also a node in the shared decision plan. */
        DecisionNodeId source; /**< May be any active ancestor. Deeper sources take precedence. */
        DecisionNodeId target;
        std::int32_t priority{}; /**< Higher wins within one source depth; ties use ascending stable transition identity. */
        std::optional<StateMachineEventId> event; /**< Absent means condition-triggered; present requires this step's event. */
        std::vector<StateMachineGuard> guards;    /**< All guards must hold; an empty conjunction is true. */
    };

    /** @brief Borrowed-by-value .horo_sm semantic source with no editor layout or runtime pointers. */
    struct StateMachineAssetDescriptor final {
        DecisionNodeId initial;
        std::vector<StateMachineState> states;
        std::vector<StateMachineTransition> transitions;
        std::size_t maximumTransitionsPerStep{1}; /**< Explicit chaining opt-in; default accepts only one transition. */
    };

    /** @brief Immutable contiguous state-machine plan layered on the unchanged shared decision header/bindings. */
    class CookedStateMachinePlan final {
    public:
        /** @brief Factory-only admission capability; callers cannot construct one. */
        class ConstructionToken final {
            ConstructionToken() = default;
            friend class CookedStateMachinePlan;
        };

        /**
         * @brief Constructs storage only after compiler admission.
         * @param token Compiler-only admission capability.
         * @param decision Retained shared decision contract.
         * @param source Validated owned semantic records.
         */
        CookedStateMachinePlan(ConstructionToken token, std::shared_ptr<const DecisionAssetPlan> decision,
                               StateMachineAssetDescriptor source)
            : decision_(std::move(decision)), source_(std::move(source)) {
            static_cast<void>(token);
        }

        CookedStateMachinePlan(const CookedStateMachinePlan &) = delete;
        CookedStateMachinePlan &operator=(const CookedStateMachinePlan &) = delete;
        /**
         * @brief Validates and resolves hierarchy, guards and priority before publication.
         * @param decision Shared compiler-admitted StateMachine plan; must contain exactly the state and transition nodes.
         * @param source Owned semantic candidate.
         * @return Immutable plan or typed topology, binding, cycle, capacity or storage failure; inputs remain unchanged on failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const CookedStateMachinePlan>> Compile(
            std::shared_ptr<const DecisionAssetPlan> decision, StateMachineAssetDescriptor source);

        /** @brief Returns the unchanged shared decision contract. @return Retained immutable plan. */
        [[nodiscard]] const DecisionAssetPlan &Decision() const noexcept {
            return *decision_;
        }

        /** @brief Returns identity-sorted semantic states. @return Plan-lifetime view. */
        [[nodiscard]] std::span<const StateMachineState> States() const noexcept {
            return source_.states;
        }

        /** @brief Returns deterministic transition records. @return Plan-lifetime view. */
        [[nodiscard]] std::span<const StateMachineTransition> Transitions() const noexcept {
            return source_.transitions;
        }

    private:
        friend class StateMachineInstance;

        [[nodiscard]] std::size_t Index(DecisionNodeId id) const noexcept;
        std::shared_ptr<const DecisionAssetPlan> decision_;
        StateMachineAssetDescriptor source_;
    };

    /** @brief Synchronous one-shot intent action phase; asynchronous task execution belongs to the shared task owner. */
    enum class StateMachineActionPhase : std::uint8_t {
        Entry,
        Update,
        Exit
    };

    /** @brief Borrowed immutable inputs for a synchronous action adapter. */
    struct StateMachineActionInvocation final {
        DecisionNodeId state;
        TaskId action;
        StateMachineActionPhase phase;
        const AiTaskOperationContext &operation;
        const BlackboardSnapshot &blackboard;
        std::uint64_t tick;
    };

    /**
     * @brief Host-composed bounded intent adapter, borrowed only during Step.
     * @details Must not retain the invocation, mutate this runner/blackboard, perform ECS topology writes,
     * wait, or start unowned work. Return failure after any irreversible partial entry and the runner becomes terminal;
     * it never pretends to restore external effects. Long-running actions enqueue into the existing cancellable task owner.
     */
    class StateMachineActions {
    public:
        StateMachineActions() = default;
        virtual ~StateMachineActions() = default;
        StateMachineActions(const StateMachineActions &) = delete;
        StateMachineActions &operator=(const StateMachineActions &) = delete;
        /**
         * @brief Emits one synchronous intent through the host's typed adapter.
         * @param invocation Borrowed immutable call-scoped inputs.
         * @return Success or an owned typed cause; never retains invocation storage.
         */
        [[nodiscard]] virtual Result<void> Invoke(const StateMachineActionInvocation &invocation) noexcept = 0;
    };

    /** @brief Bounded evaluation disposition, not a competing task terminal-status vocabulary. */
    enum class StateMachineStepDisposition : std::uint8_t {
        Stable,
        Transitioned,
        CycleBounded,
        BudgetBounded,
        Terminal
    };

    /** @brief Stable-identity evidence from one evaluation step. */
    struct StateMachineStepResult final {
        StateMachineStepDisposition disposition{StateMachineStepDisposition::Stable};
        DecisionNodeId activeState;
        DecisionNodeId lastTransition;
        std::size_t transitions{};
    };

    /**
     * @brief Scene-owned hierarchical state runner with fixed-size dynamic state and shared cancellation lifecycle.
     * @details Only the authorized simulation owner calls Step in AiDecisionEvaluate, after BlackboardSync.
     * This kernel does not grant authority, schedule itself, choose network roles or own Scene objects.
     * At this implementation boundary AIDecisionSystem/BehaviorExecutionContext are not yet concrete code.
     * The host supplies the exact current agent/binding and a frozen snapshot, and cancels before owner retirement.
     * Actions execute ancestor-first entry/update and descendant-first exit. Failed entry/exit/update produces a
     * shared Failed terminal result and clears the logical active path, never an incoherent partly active state.
     */
    class StateMachineInstance final {
    public:
        /** @brief Factory-only runtime admission capability; callers cannot construct one. */
        class ConstructionToken final {
            ConstructionToken() = default;
            friend class StateMachineInstance;
        };

        /**
         * @brief Constructs bounded instance storage after factory admission.
         * @param token Factory-only admission capability.
         * @param plan Retained immutable plan.
         * @param binding Copied exact generation fence.
         */
        StateMachineInstance(ConstructionToken token, std::shared_ptr<const CookedStateMachinePlan> plan,
                             const BlackboardInstanceBinding &binding)
            : plan_(std::move(plan)), binding_(binding) {
            static_cast<void>(token);
        }

        /**
         * @brief Activates bounded storage without invoking actions.
         * @param plan Retained admitted plan.
         * @param binding Exact current agent/schema/publication/instance generations.
         * @param operation Shared task-lifecycle admission context for this runner.
         * @return Owned instance or typed admission/storage failure. Pre-cancellation is terminal without entry.
         */
        [[nodiscard]] static Result<std::unique_ptr<StateMachineInstance>> Create(std::shared_ptr<const CookedStateMachinePlan> plan,
                                                                                  const BlackboardInstanceBinding &binding,
                                                                                  AiTaskOperationContext operation);
        /**
         * @brief Runs one bounded evaluation step against one immutable input set.
         * @param tick Strictly increasing committed simulation tick; repeated/backward ticks are rejected.
         * @param current Exact current blackboard binding observed by the owner.
         * @param blackboard Frozen snapshot matching current; expired generations cannot run actions.
         * @param events Step-scoped typed gameplay event identities, at most EventsPerStep; not retained or consumed twice by chaining.
         * @param actions Step-scoped host intent adapter. Required if any admitted state has an action.
         * @return Stable-identity evidence or typed failure. Chaining stops before revisiting a state and at the declared bound.
         */
        [[nodiscard]] Result<StateMachineStepResult> Step(std::uint64_t tick, const BlackboardInstanceBinding &current,
                                                          const BlackboardSnapshot &blackboard,
                                                          std::span<const StateMachineEventId> events = {},
                                                          StateMachineActions *actions = nullptr);
        /** @brief Cancels without invoking further actions. @param reason Shared cancellation cause. @return Shared disposition. */
        [[nodiscard]] Result<AiTaskTransitionDisposition> Cancel(AiTaskCancellationReason reason);

        /** @brief Returns the shared immutable terminal outcome. @return Null before cancellation/failure. */
        [[nodiscard]] const AiTaskTerminalResult *TerminalResult() const noexcept {
            return lifecycle_.TerminalResult();
        }

        /** @brief Returns the deepest active state or invalid when unstarted/terminal. @return Stable authored identity. */
        [[nodiscard]] DecisionNodeId ActiveState() const noexcept;
        StateMachineInstance(const StateMachineInstance &) = delete;
        StateMachineInstance &operator=(const StateMachineInstance &) = delete;

    private:
        using Path = std::array<std::size_t, StateMachineLimits::Depth>;
        [[nodiscard]] std::size_t MakePath(DecisionNodeId target, Path &path) const noexcept;
        [[nodiscard]] Result<void> CheckBoundary(const BlackboardSnapshot &blackboard);
        [[nodiscard]] bool ValidStep(std::uint64_t tick, std::span<const StateMachineEventId> events) const noexcept;
        [[nodiscard]] bool MatchesInput(const BlackboardInstanceBinding &current, const BlackboardSnapshot &blackboard) const;
        [[nodiscard]] bool HasRequiredActions(const StateMachineActions *actions) const noexcept;
        [[nodiscard]] Result<StateMachineStepResult> AdvanceTransitions(std::uint64_t tick, const BlackboardSnapshot &blackboard,
                                                                        std::span<const StateMachineEventId> events,
                                                                        StateMachineActions *actions);
        [[nodiscard]] Result<void> RunActions(const Path &path, std::size_t begin, std::size_t end, StateMachineActionPhase phase,
                                              const BlackboardSnapshot &blackboard, std::uint64_t tick, StateMachineActions *actions);
        [[nodiscard]] Result<bool> GuardsHold(const StateMachineTransition &transition, const BlackboardSnapshot &blackboard) const;
        [[nodiscard]] Result<const StateMachineTransition *> Select(const BlackboardSnapshot &blackboard,
                                                                    std::span<const StateMachineEventId> events) const;
        [[nodiscard]] Result<void> Enter(DecisionNodeId target, const BlackboardSnapshot &blackboard, std::uint64_t tick,
                                         StateMachineActions *actions, std::optional<std::size_t> source = {});
        [[nodiscard]] Result<StateMachineStepResult> Fail(Error error);
        std::shared_ptr<const CookedStateMachinePlan> plan_;
        BlackboardInstanceBinding binding_;
        AiTaskLifecycle lifecycle_;
        Path path_{};
        std::size_t depth_{};
        std::optional<std::uint64_t> lastTick_;
        bool evaluating_{};
    };
}  // namespace Horo::AI
