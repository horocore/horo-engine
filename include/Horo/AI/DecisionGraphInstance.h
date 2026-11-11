#pragma once
/** @file DecisionGraphInstance.h
 * @brief Host-driven BT/HSM/Utility composition with independently allocated call occurrences and no scheduler ownership.
 */
#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/AI/DecisionStepTrace.h"

namespace Horo::AI {
    /** @brief Actual owner-issued range and operation identity; compilation never invents task/provider identities. */
    struct DecisionGraphInstanceBinding final {
        BlackboardInstanceBinding blackboard;
        AiTaskOperationContext operation;
        std::uint32_t firstTaskSlot{};
        std::uint64_t availableTaskSlots{};
        std::uint32_t firstTaskGeneration{1};
    };

    /** @brief One committed host observation. Every member call sees the same frozen agent blackboard and tick.
     * Current M3 calls require the same admitted agent schema across the closure; mismatched schemas fail compilation.
     * There is no implicit key conversion, mutable-document access or hidden blackboard owner.
     */
    struct DecisionGraphStepInput final {
        std::uint64_t tick{};
        BlackboardInstanceBinding current;
        const BlackboardSnapshot &blackboard;
        std::span<const StateMachineEventId> events;
        AiWorkBudget *utilityAgentBudget{};  /**< Required for closures containing Utility. Borrowed for this Step only. */
        AiWorkBudget *utilityGlobalBudget{}; /**< Host-issued global allowance, shared across all evaluated occurrences. */
    };

    /** @brief One complete executable allocation; actual decision kernels own their existing traversal/task semantics.
     * @details Create preallocates every possible call occurrence, including mutually exclusive HSM states and
     * parallel repeated references. Only selected occurrences enter execution. The host calls Step at its existing
     * AI decision boundary; this object creates no jobs, frame callbacks, Scene authority or task manager.
     */
    class DecisionGraphInstance final {
    public:
        [[nodiscard]] static Result<std::unique_ptr<DecisionGraphInstance>> Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                   std::shared_ptr<const DecisionNodeGeneration> providers,
                                                                                   DecisionGraphInstanceBinding binding);
        /** @brief Logical composition-adapter allocation per occurrence; family kernels report their own extents. */
        [[nodiscard]] static std::uint64_t CoreStorageBytes(const DecisionCompiledGraph &graph) noexcept;
        [[nodiscard]] Result<AiTaskState> Step(const DecisionGraphStepInput &input);
        /** @brief Advances the same runtime with opt-in bounded, redacted decision evidence.
         * @param input The committed tick and immutable blackboard snapshot used by every occurrence.
         * @param trace Caller-owned destination. Invalid, off-owner or reentrant admission leaves it untouched.
         * @return The same lifecycle outcome or original typed failure as Step.
         */
        [[nodiscard]] Result<AiTaskState> StepWithTrace(const DecisionGraphStepInput &input, DecisionStepTrace &trace);
        [[nodiscard]] Result<void> Cancel(AiTaskCancellationReason reason);
        ~DecisionGraphInstance();

    private:
        struct OccurrenceState;
        struct CallAdapter;
        struct ActionAdapter;
        struct UtilityAdapter;
        /** @brief Shares the exact evaluation path for traced and untraced steps. */
        [[nodiscard]] Result<AiTaskState> StepImpl(const DecisionGraphStepInput &input, DecisionStepTrace *trace);
        /** @brief Runs one admitted step using the existing evaluation and cancellation boundaries. */
        [[nodiscard]] Result<AiTaskState> StepValidated(const DecisionGraphStepInput &input, DecisionStepTrace *trace);
        /** @brief Correlates a record to the exact compiled call and its stable caller. */
        [[nodiscard]] DecisionTraceOccurrence TraceOccurrence(std::size_t index) const noexcept;
        /** @brief Copies the state and last transition reported by the actual HSM kernel. */
        void TraceMachine(std::size_t index, const StateMachineStepResult &step) const noexcept;
        [[nodiscard]] static Result<void> Admit(const std::shared_ptr<const CookedDecisionPlan> &plan,
                                                const std::shared_ptr<const DecisionNodeGeneration> &providers,
                                                const DecisionGraphInstanceBinding &binding);
        /** @brief Requires host allowances only for closures that contain Utility. */
        [[nodiscard]] bool UtilityBudgetsAvailable(const DecisionGraphStepInput &input) const noexcept;
        [[nodiscard]] Result<void> ValidateStep(const DecisionGraphStepInput &input) const;
        [[nodiscard]] Result<void> BuildOccurrence(std::size_t index);
        [[nodiscard]] Result<void> ResetOccurrence(std::size_t index);
        [[nodiscard]] Result<AiTaskState> Advance(std::size_t index);
        /** @brief Advances one admitted state-machine occurrence and captures its optional step evidence. */
        /** @brief Advances one Utility occurrence using its canonical task kernel and host allowances. */
        [[nodiscard]] Result<AiTaskState> AdvanceUtility(std::size_t index);
        /** @brief Prepares Utility action runners with disjoint owner-issued task slots. */
        [[nodiscard]] Result<void> BuildUtility(std::size_t index);
        [[nodiscard]] Result<AiTaskState> AdvanceMachine(std::size_t index);
        [[nodiscard]] Result<AiTaskState> Call(std::size_t parent, DecisionNodeId node, bool entering);
        [[nodiscard]] Result<void> BeginCall(std::size_t parent, DecisionNodeId node);
        void EndCall(std::size_t parent, DecisionNodeId node, AiTaskCancellationReason reason) noexcept;
        void StopOccurrence(std::size_t index, AiTaskCancellationReason reason) noexcept;
        [[nodiscard]] std::size_t FindChild(std::size_t parent, DecisionNodeId node) const noexcept;
        [[nodiscard]] AiTaskOperationContext Operation(std::size_t index, TaskId task, std::uint32_t offset,
                                                       std::uint32_t generation) const;
        [[nodiscard]] bool Available() const noexcept;
        std::shared_ptr<const CookedDecisionPlan> plan_;
        std::shared_ptr<const DecisionNodeGeneration> providers_;
        DecisionGraphInstanceBinding binding_;
        std::vector<std::unique_ptr<OccurrenceState>> occurrences_;
        std::thread::id owner_;
        bool usesUtility_{};
        bool evaluating_{};
        bool shutdown_{};
        std::uint64_t lastTick_{};
        const DecisionGraphStepInput *input_{};
        DecisionStepTrace *trace_{};
    };

    /** @brief Planned canonical HSM additions, to be placed on the existing source/runner types, not a parallel runner.
     * StateMachineState owns optional DecisionSubplanCall plus explicit DecisionStateTerminal.
     * StateMachineTransition owns optional child AiTaskState guard, valid only for a source state with a call.
     * Entry: ancestor-first entry action, then that state's child starts on its update boundary.
     * Update: normal action followed by one child step, before transition selection.
     * Exit: descendant-first child cancellation/cleanup precedes that state's exit action.
     * Terminal state: complete entry first, publish its explicit outcome without further update/transition work.
     * Owner cancellation: cancel/cleanup children, invoke no new state entry/update/exit actions after revocation.
     * Re-entry: reset only that occurrence subtree and advance existing nonwrapping task generations.
     */
}  // namespace Horo::AI
