#pragma once

/** @file UtilityDecision.h
 * @brief Typed Utility graph authoring and immutable action plans.
 */

#include "Horo/AI/BehaviorTree.h"
#include "Horo/AI/BehaviorTreeRuntime.h"
#include "Horo/AI/UtilitySelector.h"

namespace Horo::AI {
    /** @brief One authored Utility option and its actual task-provider binding. */
    struct UtilityDecisionOption final {
        UtilityOption option;
        DecisionAssetNode provider; /**< Must carry the same stable node identity as option.id. */
        std::vector<BehaviorTreeProperty> properties;
    };

    /** @brief Typed Utility graph source. Each option owns one provider task and bounded considerations. */
    struct UtilityDecisionSource final {
        DecisionNodeId root;
        std::vector<DecisionBlackboardRequirement> requirements; /**< Complete declared scalar reads of the selector root. */
        std::vector<UtilityDecisionOption> options;
    };

    /** @brief Captured selector and canonical task kernels, ordered by stable option identity.
     * @details One option is selected on activation and retained until its task completes or is cancelled.
     * Tasks use the existing BehaviorTree task lifecycle, continuation mailbox and provider adapter.
     */
    class CookedUtilityPlan final {
    public:
        /** @brief Validates and compiles one Utility graph against its admitted schema and providers.
         * @param admission Complete-closure compiler proof. @param bindings Admitted graph bindings.
         * @param source Bounded typed options. @return Complete plan or a typed schema/storage failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const CookedUtilityPlan>> CompileClosure(
            DecisionClosureAdmission admission, std::shared_ptr<const DecisionAssetPlan> bindings, const UtilityDecisionSource &source);
        /** @brief Returns the immutable selector. @return Schema-bound option policies. */
        [[nodiscard]] const UtilitySelector &Selector() const noexcept;
        /** @brief Returns task plans in selector option-ID order. @return Owned immutable task kernels. */
        [[nodiscard]] std::span<const std::shared_ptr<const BehaviorTreeExecutionPlan>> Actions() const noexcept;
        /** @brief Returns the authored selector root identity. @return Root node. */
        [[nodiscard]] DecisionNodeId Root() const noexcept;
        /** @brief Measures bounded logical storage for the selector and all preallocated task runners. @return Storage bounds. */
        [[nodiscard]] DecisionKernelStorage Storage() const noexcept;

    private:
        /** @brief Owns a validated selector under the authored root identity. */
        CookedUtilityPlan(DecisionNodeId root, UtilitySelector selector);
        DecisionNodeId root_;
        UtilitySelector selector_;
        std::vector<std::shared_ptr<const BehaviorTreeExecutionPlan>> actions_;
    };
}  // namespace Horo::AI
