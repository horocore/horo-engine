#include "Horo/AI/DecisionGraphInstance.h"

namespace Horo::AI {
    /** @copydoc DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan>,std::shared_ptr<const
     * DecisionNodeGeneration>,AiDecisionTaskReservation,AiTaskOperationContext) */
    Result<std::unique_ptr<DecisionGraphInstance>> DecisionGraphInstance::Create(std::shared_ptr<const CookedDecisionPlan> plan,
                                                                                 std::shared_ptr<const DecisionNodeGeneration> providers,
                                                                                 AiDecisionTaskReservation reservation,
                                                                                 AiTaskOperationContext operation) {
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
        auto created = Create(std::move(plan), std::move(providers), std::move(binding));
        if (created.HasError())
            return created;
        if (!reservation.CanAdmit() || !created.Value()->Available())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        created.Value()->reservation_.emplace(std::move(reservation));
        return created;
    }
}  // namespace Horo::AI
