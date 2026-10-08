#pragma once

/** @file DecisionWakePolicy.h
 * @brief Owner-thread reactive decision subscriptions and finite fixed-tick polling admission.
 */

#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/DecisionAssetValidation.h"
#include "Horo/Foundation/CancellationToken.h"

namespace Horo::AI {
    /** @brief Coalesced causes for one compiled node; causes carry no mutable gameplay payload. */
    struct DecisionWakeReasons final {
        bool activation{};
        bool blackboard{};
        bool perception{};
        bool task{};
        bool polling{};
        bool explicitRequest{};
        /** @brief Checks whether at least one cause is present. @return Whether the node needs evaluation. */
        [[nodiscard]] bool Any() const noexcept;
    };

    /** @brief Stable node identity and coalesced causes delivered in ascending node order. */
    struct DecisionWakeRequest final {
        DecisionNodeId node;
        DecisionWakeReasons reasons;
    };

    /** @brief Explicit polling/service cadence, lowered by the host from executable plan semantics. */
    struct DecisionWakePollingRule final {
        DecisionNodeId node;
        std::uint64_t firstTick{1};     /**< First positive absolute fixed tick eligible for polling. */
        std::uint64_t intervalTicks{1}; /**< Positive cadence; missed intervals coalesce without catch-up loops. */
        bool enabled{true};             /**< The host enables service cadences only while their owning subtree is active. */
    };

    /** @brief Compiler-admitted node's declared dependency on a typed perception listener. */
    struct DecisionWakePerceptionDependency final {
        DecisionNodeId node;
        PerceptionListenerTypeId listener;
    };

    /** @brief Lowerable deterministic work admission; wall-clock timing is never used. */
    struct DecisionWakeBudget final {
        static constexpr std::size_t HardEvaluationsPerTick = 8;
        std::size_t maximumEvaluationsPerTick{1};       /**< Includes events published during evaluation at this same tick. */
        std::size_t maximumPollingWakeupsPerUpdate{32}; /**< Due rules admitted per update, using a stable rotating cursor. */
    };

    /**
     * @brief One graph's scene-owner wake policy; the decision system remains the sole evaluator/scheduler.
     * @details All methods, observer callbacks and destruction use the BlackboardSync/AiDecisionEvaluate owner thread.
     * The borrowed blackboard must outlive this object. Destroy or CloseAtBlackboardSync outside blackboard publication.
     * Admission returns causes only: it invokes no provider, creates no jobs and reads no gameplay payload.
     * The host drains detached task outcomes and committed perception events before admission, routes exact-generation
     * notifications here, then evaluates against one frozen snapshot. Worker producers must never call this object.
     */
    class DecisionWakePolicy final {
    public:
        /**
         * @brief Validates finite metadata and subscribes each distinct compiled key at the owner safe point.
         * @param plan Immutable admitted decision bindings retained for the subscription lifetime.
         * @param blackboard Borrowed exact active schema publication; must outlive this policy.
         * @param observerOwner Host-reserved task handle whose lifetime covers the whole graph subscription.
         * @param polling Explicit periodic services/polls, at most one rule per admitted node.
         * @param perception Declared typed listener dependencies, bounded by the admitted node ceiling.
         * @param budget Positive lowerable polling/evaluation caps.
         * @param cancellation Scene/graph lifetime cancellation observed before every wake/admission.
         * @return Owned policy or typed validation/capacity/storage failure; partial subscriptions are removed.
         */
        [[nodiscard]] static Result<std::unique_ptr<DecisionWakePolicy>> CreateAtBlackboardSync(
            std::shared_ptr<const DecisionAssetPlan> plan, BlackboardInstance &blackboard, TaskHandle observerOwner,
            std::span<const DecisionWakePollingRule> polling = {}, std::span<const DecisionWakePerceptionDependency> perception = {},
            const DecisionWakeBudget &budget = {}, CancellationToken cancellation = {});
        /**
         * @brief Freezes one finite wake batch for the sole decision owner.
         * @param tick Positive nondecreasing simulation tick; a new tick replenishes the evaluation cap.
         * @param activeAgent Current agent generation; stale input closes this policy before admission.
         * @return Borrowed stable-node-ordered requests, empty for idle/budget-deferred work, or typed lifecycle failure.
         * @details A nonempty batch must be paired with FinishUpdate. Reentry is rejected. Notifications received
         * during evaluation remain pending for a later bounded admission. Idle calls do not scan nodes/polls or allocate.
         * The returned span expires on FinishUpdate, CloseAtBlackboardSync or destruction.
         */
        [[nodiscard]] Result<std::span<const DecisionWakeRequest>> BeginUpdate(std::uint64_t tick, AgentHandle activeAgent);
        /** @brief Ends one nonempty frozen admission. @return Success or typed unmatched/reentrant lifecycle failure. */
        [[nodiscard]] Result<void> FinishUpdate();
        /**
         * @brief Enables or suspends an admitted cadence as its owning subtree becomes active/inactive.
         * @param node Compiled node with an admitted polling rule. @param enabled Whether this cadence is active.
         * @param nextTick Positive absolute next deadline when enabling; must not precede the last update tick.
         * @return Success or typed rule/clock/lifecycle failure. Disabling does not retract an already frozen wake batch.
         */
        [[nodiscard]] Result<void> SetPollingEnabled(DecisionNodeId node, bool enabled, std::uint64_t nextTick);
        /** @brief Queues an explicit compiled-node wake. @param node Admitted identity. @return Success or typed missing/closed failure. */
        [[nodiscard]] Result<void> Request(DecisionNodeId node);
        /**
         * @brief Coalesces a committed perception event for declared listener dependencies only.
         * @param agent Exact recipient generation. @param listener Declared listener identity.
         * @return Whether any dependency matched, or typed stale/closed failure.
         */
        [[nodiscard]] Result<bool> NotifyPerception(AgentHandle agent, PerceptionListenerTypeId listener);
        /**
         * @brief Binds or revokes one node's exact current task generation; does not start a task.
         * @param node Admitted identity. @param task Exact live handle, or empty to revoke.
         * @return Success or typed invalid/foreign/duplicate task binding failure.
         */
        [[nodiscard]] Result<void> BindTask(DecisionNodeId node, const std::optional<TaskHandle> &task);
        /**
         * @brief Coalesces an owner-drained task event/terminal without applying its payload.
         * @param agent Exact owning generation. @param node Admitted identity. @param task Current bound execution handle.
         * @return Whether the execution matched; retired generations return false, stale owner/closed input fails.
         */
        [[nodiscard]] Result<bool> NotifyTask(AgentHandle agent, DecisionNodeId node, TaskHandle task);
        /** @brief Removes owned observers and fences future wakes. @return Success or blackboard publication-reentrancy failure. */
        [[nodiscard]] Result<void> CloseAtBlackboardSync();
        /** @brief Removes observers on the owner thread before releasing callback storage. */
        ~DecisionWakePolicy();
        DecisionWakePolicy(const DecisionWakePolicy &) = delete;
        DecisionWakePolicy &operator=(const DecisionWakePolicy &) = delete;
        DecisionWakePolicy(DecisionWakePolicy &&) = delete;
        DecisionWakePolicy &operator=(DecisionWakePolicy &&) = delete;

    private:
        struct NodeState final {
            DecisionWakeReasons pending;
            std::optional<TaskHandle> task;
        };

        struct Watch final {
            DecisionWakePolicy *owner{};
            BlackboardKeyId key;
            std::vector<std::size_t> nodes;
            std::optional<BlackboardObserverToken> token;
        };

        struct Poll final {
            std::size_t node{};
            std::uint64_t next{};
            std::uint64_t interval{};
            bool exhausted{};
            bool enabled{true};
        };

        DecisionWakePolicy() = default;
        /** @brief Resolves one admitted stable identity. @param node Persistent identity. @return Offset or node count. */
        [[nodiscard]] std::size_t Find(DecisionNodeId node) const noexcept;
        /** @brief Checks active blackboard/agent publication without copying a snapshot. @return Generation validity. */
        [[nodiscard]] bool Current() const noexcept;
        /** @brief Converts one already key-filtered notification to coalesced node causes. */
        static void OnBlackboard(void *context, const BlackboardNotificationBatch &notification) noexcept;
        /** @brief Admits a bounded due-rule prefix and recomputes the next idle deadline. @param tick Fixed simulation time. */
        void AdmitPolling(std::uint64_t tick) noexcept;
        /** @brief Caches the earliest enabled non-exhausted cadence for constant-work idle admission. */
        void RefreshDeadline() noexcept;
        /** @brief Prepares immutable node/key edges and finite timer state. @return Validation result. */
        [[nodiscard]] Result<void> Prepare(std::span<const DecisionWakePollingRule> polling,
                                           std::span<const DecisionWakePerceptionDependency> perception);
        /** @brief Builds distinct key watches and their compiled node edges before observer registration. */
        void PrepareWatches();
        /** @brief Admits stable-node-ordered unique cadences. @param polling Declared rules. @return Typed admission result. */
        [[nodiscard]] Result<void> PreparePolling(std::span<const DecisionWakePollingRule> polling);
        /** @brief Admits listener/node ordered unique dependencies. @param perception Declared dependencies. @return Admission result. */
        [[nodiscard]] Result<void> PreparePerception(std::span<const DecisionWakePerceptionDependency> perception);

        std::shared_ptr<const DecisionAssetPlan> plan_;
        BlackboardInstance *blackboard_{};
        BlackboardInstanceBinding binding_;
        DecisionWakeBudget budget_;
        CancellationToken cancellation_;
        std::vector<NodeState> nodes_;
        std::vector<Watch> watches_;
        std::vector<Poll> polls_;
        std::vector<DecisionWakePerceptionDependency> perception_;
        std::vector<DecisionWakeRequest> admitted_;
        std::uint64_t tick_{};
        std::uint64_t nextPoll_{};
        std::size_t evaluations_{};
        std::size_t pollCursor_{};
        bool pending_{true};
        bool hasPoll_{};
        bool evaluating_{};
        bool closed_{};
    };
}  // namespace Horo::AI
