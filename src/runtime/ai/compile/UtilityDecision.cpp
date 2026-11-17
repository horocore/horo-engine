#include "Horo/AI/UtilityDecision.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        /** @brief Requires every score read to name one admitted scalar binding on the selector root. */
        bool BoundOption(const UtilityDecisionOption &option, const DecisionAssetPlan &bindings, const DecisionNodeId root) {
            if (option.option.id != option.provider.id || option.provider.id == root)
                return false;
            const auto rootBindings = bindings.BindingsForNode(root);
            return std::ranges::all_of(option.option.considerations, [rootBindings](const auto &consideration) {
                return std::ranges::find(rootBindings, consideration.key, &DecisionPlanBlackboardBinding::key) != rootBindings.end();
            });
        }
    }  // namespace

    /** @copydoc CookedUtilityPlan::CookedUtilityPlan */
    CookedUtilityPlan::CookedUtilityPlan(const DecisionNodeId root, UtilitySelector selector)
        : root_(root), selector_(std::move(selector)) {}

    /** @copydoc CookedUtilityPlan::CompileClosure */
    Result<std::shared_ptr<const CookedUtilityPlan>> CookedUtilityPlan::CompileClosure(const DecisionClosureAdmission admission,
                                                                                       std::shared_ptr<const DecisionAssetPlan> bindings,
                                                                                       const UtilityDecisionSource &source) {
        using Plan = Result<std::shared_ptr<const CookedUtilityPlan>>;
        if (!bindings || bindings->Kind() != DecisionPlanKind::Utility || !source.root.IsValid() ||
            source.options.size() > UtilitySelector::MaximumOptions)
            return Plan::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        std::vector<UtilityOption> options;
        options.reserve(source.options.size());
        for (const auto &option : source.options) {
            if (!BoundOption(option, *bindings, source.root))
                return Plan::Failure(MakeError(AIErrors::DecisionAssetBindingMissing));
            options.push_back(option.option);
        }
        auto selector = UtilitySelector::Capture(bindings->BlackboardSchema(), options);
        if (selector.HasError())
            return Plan::Failure(selector.ErrorValue());
        auto result = std::shared_ptr<CookedUtilityPlan>(new CookedUtilityPlan(source.root, std::move(selector).Value()));
        result->actions_.reserve(source.options.size());
        for (const auto &option : result->selector_.Options()) {
            auto action = BehaviorTreeExecutionPlan::CompileUtilityAction(admission, bindings, option.id, option.action);
            if (action.HasError())
                return Plan::Failure(action.ErrorValue());
            result->actions_.push_back(std::move(action).Value());
        }
        return Plan::Success(std::move(result));
    }

    /** @copydoc CookedUtilityPlan::Selector */
    const UtilitySelector &CookedUtilityPlan::Selector() const noexcept {
        return selector_;
    }

    /** @copydoc CookedUtilityPlan::Actions */
    std::span<const std::shared_ptr<const BehaviorTreeExecutionPlan>> CookedUtilityPlan::Actions() const noexcept {
        return actions_;
    }

    /** @copydoc CookedUtilityPlan::Root */
    DecisionNodeId CookedUtilityPlan::Root() const noexcept {
        return root_;
    }

    /** @copydoc CookedUtilityPlan::Storage */
    DecisionKernelStorage CookedUtilityPlan::Storage() const noexcept {
        auto storage = selector_.Storage();
        storage.compiledBytes += sizeof(CookedUtilityPlan) - sizeof(UtilitySelector) +
                                 actions_.capacity() * sizeof(std::shared_ptr<const BehaviorTreeExecutionPlan>);
        for (const auto &action : actions_) {
            const auto task = action->Storage();
            storage.compiledBytes += task.compiledBytes;
            storage.instanceBytes += task.instanceBytes;
            storage.scratchBytes += task.scratchBytes;
            storage.taskSlots += task.taskSlots;
        }
        storage.maximumDepth = 2;
        return storage;
    }
}  // namespace Horo::AI
