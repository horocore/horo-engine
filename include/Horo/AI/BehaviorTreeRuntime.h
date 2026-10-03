#pragma once

/** @file BehaviorTreeRuntime.h
 * @brief Bounded deterministic execution of compiled behavior-tree control flow.
 */

#include "Horo/AI/AITaskLifecycle.h"
#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/DecisionAssetValidation.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::AI {
    /** @brief Executable core operations, independent of provider/display names. */
    enum class BehaviorTreeOperation : std::uint8_t {
        Sequence,
        Selector,
        Parallel,
        Inverter,
        Cooldown,
        Loop,
        BlackboardCheck,
        TimeLimit,
        Task,
        Count,
    };
    /** @brief Parallel completion policy; branches are stepped in declared order on the owner thread. */
    enum class BehaviorTreeParallelPolicy : std::uint8_t {
        RequireOneSuccess,
        RequireAllSuccess,
        RequireAllComplete,
        StopOthersOnFailure,
        Count,
    };
    /** @brief Reactive condition policy; lower-priority observers must be direct Selector children. */
    enum class BehaviorTreeAbortMode : std::uint8_t {
        None,
        Self,
        LowerPriority,
        Both,
        Count
    };
    /** @brief Service wake policy while its owning subtree is active. */
    enum class BehaviorTreeServiceMode : std::uint8_t {
        Periodic,
        Reactive,
        Count
    };

    /** @brief Typed execution semantics bound to an already validated stable decision node. */
    struct BehaviorTreeExecutionNode final {
        DecisionNodeId id;
        BehaviorTreeOperation operation{BehaviorTreeOperation::Task};
        std::vector<DecisionNodeId> children; /**< Semantic priority/order, independent of source array order. */
        BehaviorTreeParallelPolicy parallel{BehaviorTreeParallelPolicy::RequireAllSuccess};
        BehaviorTreeAbortMode abort{BehaviorTreeAbortMode::None};
        std::uint64_t durationTicks{}; /**< Cooldown/TimeLimit duration; TimeLimit requires a positive duration. */
        std::uint32_t iterations{1};   /**< Positive finite Loop count; at most one iteration completes per evaluation. */
        TaskId task;                   /**< Persistent task definition; required only for Task. */
        [[nodiscard]] bool operator==(const BehaviorTreeExecutionNode &) const = default;
    };

    /** @brief Service attachment with its own validated decision-node binding. */
    struct BehaviorTreeExecutionService final {
        DecisionNodeId id;
        DecisionNodeId owner;
        BehaviorTreeServiceMode mode{BehaviorTreeServiceMode::Periodic};
        std::uint64_t intervalTicks{1}; /**< Positive periodic interval. Reactive services wake on committed revision changes. */
        [[nodiscard]] bool operator==(const BehaviorTreeExecutionService &) const = default;
    };

    /** @brief Project-lowerable finite execution limits; no runtime input can raise the hard ceiling. */
    struct BehaviorTreeExecutionLimits final {
        static constexpr std::size_t HardNodes = DecisionAssetValidationHardLimits::NodesPerAsset;
        std::size_t maximumNodes{HardNodes};
        std::size_t maximumDepth{HardNodes};
    };

    /** @brief Preorder control-flow record with contiguous descendant and attachment ranges. */
    struct BehaviorTreePlanNode final {
        BehaviorTreeExecutionNode execution;
        std::size_t parent{};
        std::size_t subtreeEnd{}; /**< Exclusive preorder end; permits bounded nonrecursive cancellation. */
        std::size_t firstChild{};
        std::size_t childCount{};
        std::size_t firstService{};
        std::size_t serviceCount{};
    };

    /** @brief Immutable executable topology retaining the shared compiler's schema/descriptor admission. */
    class BehaviorTreeExecutionPlan final {
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class BehaviorTreeExecutionPlan;
        };

    public:
        /** @brief Constructs inert plan storage; the private key restricts admission to Compile. */
        explicit BehaviorTreeExecutionPlan(ConstructionKey) {}

        /**
         * @brief Compiles typed control flow against an admitted decision plan without invoking providers.
         * @param bindings Immutable BehaviorTree decision plan; all control and service identities must occur exactly once.
         * @param root Stable root control identity.
         * @param nodes Typed control nodes, in any source order.
         * @param services Service attachments in declared execution order.
         * @param limits Lowerable node and structural depth limits.
         * @return Complete immutable plan or typed schema/topology/capacity/storage failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const BehaviorTreeExecutionPlan>> Compile(
            std::shared_ptr<const DecisionAssetPlan> bindings, DecisionNodeId root, std::span<const BehaviorTreeExecutionNode> nodes,
            std::span<const BehaviorTreeExecutionService> services = {}, const BehaviorTreeExecutionLimits &limits = {});

        /** @brief Returns preorder records. @return Immutable contiguous nodes. */
        [[nodiscard]] std::span<const BehaviorTreePlanNode> Nodes() const noexcept {
            return nodes_;
        }

        /** @brief Returns indexed child ranges. @return Immutable contiguous child offsets. */
        [[nodiscard]] std::span<const std::size_t> Children() const noexcept {
            return children_;
        }

        /** @brief Returns owner-grouped service attachments. @return Immutable service records. */
        [[nodiscard]] std::span<const BehaviorTreeExecutionService> Services() const noexcept {
            return services_;
        }

        /** @brief Returns the original admitted binding plan. @return Owned immutable schema/descriptor lease. */
        [[nodiscard]] const DecisionAssetPlan &Bindings() const noexcept {
            return *bindings_;
        }

        /** @brief Returns the validated structural depth. @return Required scratch-frame capacity. */
        [[nodiscard]] std::size_t Depth() const noexcept {
            return depth_;
        }

        /** @brief Tests conservative reload compatibility by stable identity and complete executable semantics.
         * @param candidate Replacement plan.
         * @return True when topology, providers, bindings, and schema publication are unchanged.
         */
        [[nodiscard]] bool IsCompatible(const BehaviorTreeExecutionPlan &candidate) const noexcept;
        BehaviorTreeExecutionPlan(const BehaviorTreeExecutionPlan &) = delete;
        BehaviorTreeExecutionPlan &operator=(const BehaviorTreeExecutionPlan &) = delete;

    private:
        /** @brief Builds preorder records after typed identity admission. @param root Stable root.
         * @param nodes Validated typed controls. @param limits Structural limits. @return Topology/depth result.
         */
        [[nodiscard]] Result<void> BuildTopology(DecisionNodeId root, std::span<const BehaviorTreeExecutionNode> nodes,
                                                 const BehaviorTreeExecutionLimits &limits);
        /** @brief Resolves child and service ranges. @param services Admitted attachments. @return Attachment ownership result. */
        [[nodiscard]] Result<void> Attach(std::span<const BehaviorTreeExecutionService> services);
        std::shared_ptr<const DecisionAssetPlan> bindings_;
        std::vector<BehaviorTreePlanNode> nodes_;
        std::vector<std::size_t> children_;
        std::vector<BehaviorTreeExecutionService> services_;
        std::size_t depth_{};
    };

    /** @brief Borrowed immutable callback inputs; providers may retain detached values only. */
    struct BehaviorTreeEvaluationContext final {
        DecisionNodeId node;
        std::span<const DecisionPlanBlackboardBinding> bindings;
        const BlackboardSnapshot &blackboard;
        std::uint64_t tick{};
        AiTaskResumeReason reason{AiTaskResumeReason::FixedTick};
    };

    /**
     * @brief Host-composed task/condition/service adapter owned by one tree instance.
     * @details Callbacks run serially in AiDecisionEvaluate, never spawn an ambient scheduler, and must not
     * reenter the instance. Service writes are staged for the next BlackboardSync, never applied to the frozen snapshot.
     * The virtual boundary is limited to provider calls; core traversal uses flat records and no virtual nodes.
     */
    class IBehaviorTreeExecutor {
    public:
        virtual ~IBehaviorTreeExecutor() = default;
        /** @brief Starts one admitted task. @param context Borrowed frozen inputs. @param operation Exact execution fence.
         * @return Running/Succeeded/Failed/Cancelled, or a typed failure retained by the lifecycle.
         */
        [[nodiscard]] virtual Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context,
                                                        const AiTaskOperationContext &operation) noexcept = 0;
        /** @brief Resumes an owned task. @param context Borrowed frozen inputs. @param operation Exact execution fence.
         * @return Running/Succeeded/Failed/Cancelled, or a typed execution failure.
         */
        [[nodiscard]] virtual Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context,
                                                         const AiTaskOperationContext &operation) noexcept = 0;
        /** @brief Cancels all downstream requests/subscriptions exactly once before cleanup.
         * @param operation Exact task fence. @param reason Published cancellation reason.
         */
        virtual void Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason reason) noexcept = 0;
        /** @brief Releases downstream resources after terminal publication, exactly once.
         * @param operation Exact task fence. @param result Immutable terminal outcome.
         */
        virtual void Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept = 0;
        /** @brief Evaluates a pure condition. @param context Frozen inputs and resolved bindings. @return Condition or typed failure. */
        [[nodiscard]] virtual Result<bool> Check(const BehaviorTreeEvaluationContext &context) noexcept = 0;
        /** @brief Runs a due service. @param context Frozen inputs and resolved bindings. @return Success or typed failure. */
        [[nodiscard]] virtual Result<void> Service(const BehaviorTreeEvaluationContext &context) noexcept = 0;
    };

    /** @brief Exact scene-owner admission for one tree's task-slot range. */
    struct BehaviorTreeInstanceBinding final {
        AgentHandle agent;
        CancellationToken cancellation;
        std::uint32_t firstTaskSlot{};        /**< Owner reserves HardNodes consecutive slots exclusively for this instance's lifetime. */
        std::uint32_t firstTaskGeneration{1}; /**< Owner-issued seed; generations never wrap, including across reload/restart. */
        BlackboardInstanceBinding blackboard; /**< Exact admitted schema publication and blackboard instance generation. */
    };

    /** @brief Single-owner allocation-bounded tree instance; scene decision scheduling remains with AIDecisionSystem. */
    class BehaviorTreeInstance final {
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class BehaviorTreeInstance;
        };

    public:
        /** @brief Constructs inert instance storage; the private key restricts admission to Create. */
        explicit BehaviorTreeInstance(ConstructionKey);

        /** @brief Creates contiguous state/scratch storage before activation.
         * @param plan Admitted executable plan. @param binding Exact owner and exclusively reserved task slots.
         * @param executor Host adapter whose ownership transfers into this instance.
         * @return Owned instance or typed admission/storage failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<BehaviorTreeInstance>> Create(std::shared_ptr<const BehaviorTreeExecutionPlan> plan,
                                                                                  BehaviorTreeInstanceBinding binding,
                                                                                  std::unique_ptr<IBehaviorTreeExecutor> executor);
        /** @brief Evaluates once using an explicit finite fixed-tick clock (events may share the current tick).
         * @param tick Nondecreasing fixed tick, positive for the first evaluation.
         * @param blackboard Frozen exact agent/schema snapshot from BlackboardSync.
         * @param activeAgent Current generation; mismatch cancels before any callback.
         * @param reason Fixed-tick or event resume boundary.
         * @return Root status or typed input/provider failure; provider errors cancel remaining owned work.
         * @details At most two traversal steps per admitted control node; loops yield between iterations.
         * Terminal roots remain terminal until Restart or incompatible Replace; no steady-state core allocation.
         */
        [[nodiscard]] Result<AiTaskState> Evaluate(std::uint64_t tick, const BlackboardSnapshot &blackboard, AgentHandle activeAgent,
                                                   AiTaskResumeReason reason = AiTaskResumeReason::FixedTick);
        /** @brief Cancels an owned subtree and every running descendant exactly once.
         * @param node Stable control node. @param reason Typed cancellation origin.
         * @return Success or typed identity/input failure. Cancellation propagates on the next evaluation.
         */
        [[nodiscard]] Result<void> AbortSubtree(DecisionNodeId node, AiTaskCancellationReason reason = AiTaskCancellationReason::Requested);
        /** @brief Restarts the root, cancelling outstanding descendants before reset. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> Restart();
        /** @brief Atomically offers a compiled replacement at the owner safe point.
         * @param candidate Complete admitted plan; null is rejected without changing the active tree.
         * @return True when compatible state was retained, false after abort/reset, or typed failure preserving the old instance.
         */
        [[nodiscard]] Result<bool> Replace(std::shared_ptr<const BehaviorTreeExecutionPlan> candidate);
        /** @brief Idempotently cancels work and fences further evaluation; also called by the destructor. */
        void Shutdown() noexcept;
        /** @brief Returns one stable node's subtree status. @param node Stable identity. @return Status or typed missing identity failure.
         */
        [[nodiscard]] Result<AiTaskState> Status(DecisionNodeId node) const;
        /** @brief Returns the root status. @return Current subtree state. */
        [[nodiscard]] AiTaskState RootStatus() const noexcept;
        /** @brief Destroys owned state after exactly-once cancellation/cleanup. */
        ~BehaviorTreeInstance();
        BehaviorTreeInstance(const BehaviorTreeInstance &) = delete;
        BehaviorTreeInstance &operator=(const BehaviorTreeInstance &) = delete;
        BehaviorTreeInstance(BehaviorTreeInstance &&) = delete;
        BehaviorTreeInstance &operator=(BehaviorTreeInstance &&) = delete;

    private:
        struct NodeState;
        struct ServiceState;
        struct Frame;
        /** @brief Projects borrowed frozen inputs and the admitted node binding range. */
        [[nodiscard]] BehaviorTreeEvaluationContext Context(DecisionNodeId node, const BlackboardSnapshot &blackboard) const noexcept;
        /** @brief Invokes active due attachments once per declared interval or revision. */
        [[nodiscard]] Result<void> RunServices(std::size_t node, const BlackboardSnapshot &blackboard);
        /** @brief Caches one pure condition result per evaluation. */
        [[nodiscard]] Result<bool> Check(std::size_t node, const BlackboardSnapshot &blackboard);
        /** @brief Aborts lower-priority owned branches before selecting a newly eligible higher-priority child. */
        [[nodiscard]] Result<void> ObservePriority(const BlackboardSnapshot &blackboard);
        /** @brief Applies entry timers, decorator guards, services and task execution. */
        [[nodiscard]] Result<void> Enter(std::size_t node, const BlackboardSnapshot &blackboard);
        /** @brief Starts or resumes one generation-fenced task and publishes its outcome. */
        [[nodiscard]] Result<void> RunTask(std::size_t node, const BlackboardSnapshot &blackboard);
        /** @brief Claims exactly-once downstream abort/cleanup for a terminal task. */
        void FinishTask(std::size_t node) noexcept;
        /** @brief Cancels every running task in one contiguous preorder descendant range. */
        void CancelRange(std::size_t node, AiTaskCancellationReason reason) noexcept;
        /** @brief Clears a retired branch while preserving logical cooldown timers. */
        void ResetRange(std::size_t node) const noexcept;
        /** @brief Propagates a completed child through its owning composite/decorator. */
        void Propagate(std::size_t parent, std::size_t child) const noexcept;
        /** @brief Applies parallel policy after the declared-order branch pass. @param node Parallel owner record. */
        void FinishParallel(std::size_t node) noexcept;
        /** @brief Executes a bounded iterative preorder traversal using preallocated depth frames. */
        [[nodiscard]] Result<AiTaskState> Traverse(const BlackboardSnapshot &blackboard);
        /** @brief Advances the owning frame after child propagation without descending again into a running child. */
        void AdvanceFrame(Frame &parent, std::size_t completed) const noexcept;
        /** @brief Resolves a stable control identity without exposing transient offsets. */
        [[nodiscard]] std::size_t Find(DecisionNodeId node) const noexcept;
        std::shared_ptr<const BehaviorTreeExecutionPlan> plan_;
        BehaviorTreeInstanceBinding binding_;
        std::unique_ptr<IBehaviorTreeExecutor> executor_;
        std::unique_ptr<NodeState[]> states_;
        std::unique_ptr<ServiceState[]> services_;
        std::unique_ptr<Frame[]> frames_;
        std::uint64_t tick_{};
        std::uint64_t evaluation_{};
        std::uint32_t nextGeneration_{};
        AiTaskResumeReason reason_{AiTaskResumeReason::FixedTick};
        bool shutdown_{};
        bool evaluating_{};
    };
}  // namespace Horo::AI
