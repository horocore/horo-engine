#include "Horo/AI/DecisionGraphInstance.h"

namespace Horo::AI {
    /** @copydoc DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan>,std::shared_ptr<const
     * DecisionNodeGeneration>,AiDecisionTaskReservation,AiTaskOperationContext) */
    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                 std::shared_ptr<const DecisionNodeGeneration> providers,
                                                                                 AiDecisionTaskReservation reservation,
                                                                                 AiTaskOperationContext operation) {
        return CreateReserved(std::move(plan), std::move(providers), std::move(reservation), std::move(operation), {});
    }

    /** @copydoc DecisionGraphInstance::CreateReserved */
    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::CreateReserved(
        std::shared_ptr<const CookedDecisionPlan> plan, std::shared_ptr<const DecisionNodeGeneration> providers,
        AiDecisionTaskReservation reservation, AiTaskOperationContext operation, std::shared_ptr<IDecisionQueryTaskBinder> queryTasks) {
        using Output = Result<std::unique_ptr<DecisionGraphInstance>>;
        if (!reservation.CanAdmit())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (!plan || plan->Occurrences().empty() || operation.agent != reservation.Binding().agent)
            return Output::Failure(MakeError(AIErrors::TaskContextInvalid));
        const auto &root = plan->Graphs()[plan->Occurrences()[0].plan];
        if (root.bindings->BlackboardSchema() != reservation.Schema())
            return Output::Failure(MakeError(AIErrors::DecisionAssetSchemaIncompatible));
        operation.ownerCancellation = reservation.Cancellation();
        DecisionGraphInstanceBinding binding{reservation.Binding(), std::move(operation), reservation.FirstSlot(), reservation.Capacity(),
                                             1};
        auto created = CreateBound(std::move(plan), std::move(providers), std::move(binding), std::move(queryTasks));
        if (created.HasError())
            return created;
        if (!reservation.CanAdmit() || !created.Value()->Available())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        created.Value()->reservation_.emplace(std::move(reservation));
        return created;
    }

    /** @copydoc DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan>,AiDecisionTaskReservation,AiTaskOperationContext)
     */
    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                 AiDecisionTaskReservation reservation,
                                                                                 AiTaskOperationContext operation) {
        return Create(std::move(plan), std::move(reservation), std::move(operation), {});
    }

    /** @copydoc DecisionGraphInstance::Create(std::shared_ptr<const
     * CookedDecisionPlan>,AiDecisionTaskReservation,AiTaskOperationContext,std::shared_ptr<IDecisionQueryTaskBinder>)
     */
    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                 AiDecisionTaskReservation reservation,
                                                                                 AiTaskOperationContext operation,
                                                                                 std::shared_ptr<IDecisionQueryTaskBinder> queryTasks) {
        auto providers = plan ? plan->providers_ : nullptr;
        return CreateReserved(std::move(plan), std::move(providers), std::move(reservation), std::move(operation), std::move(queryTasks));
    }
}  // namespace Horo::AI
