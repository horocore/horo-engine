#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"

#include <array>

namespace {
    /** @brief Exercises scheduler calls through the staged public headers and static library. */
    int ExerciseScheduler(Horo::AI::AiTaskScheduler &scheduler) {
        if (scheduler.SetRelevanceBatchAtSafePoint({}).HasValue())
            return 7;
        auto staged = scheduler.PreparePopulationChange({}, {});
        if (staged.HasError() || staged.Value()->ValidatePublication().HasError())
            return 6;
        staged.Value()->Publish();
        staged.Value()->FinishRetirements();
        if (scheduler.EvaluateAtDecision(1).HasError() || scheduler.CommitAtIntentDispatch(1).HasError())
            return 4;
        scheduler.Shutdown();
        return 0;
    }
}  // namespace

int main() {
    Horo::AI::AiWorkBudget budget{2};
    if (!budget.Consume(2) || budget.Consume())
        return 1;
    Horo::JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 2});
    auto runtime = Horo::AI::AiSceneRuntime::Create();
    auto incarnation = Horo::AI::AiRuntimeIncarnation::Create(1);
    if (runtime.HasError() || incarnation.HasError())
        return 2;
    auto scheduler = Horo::AI::AiTaskScheduler::Create(jobs, incarnation.Value());
    if (scheduler.HasError())
        return 3;
    if (const auto checked = ExerciseScheduler(*scheduler.Value()); checked != 0)
        return checked;
    std::array<Horo::AI::QueryGeneratedItem, 1> candidates;
    const Horo::AI::QueryGeneratorOutput output{{Horo::AI::QueryStageId::Create(1).Value(), Horo::AI::QueryGeneratorId::Create(2).Value(),
                                                 Horo::AI::QueryProviderId::Create(3).Value(), 1},
                                                candidates.size(),
                                                candidates};
    const auto generated = Horo::AI::GenerateQueryPoints({}, output);
    if (generated.HasError() || generated.Value().itemCount != 0)
        return 5;
    return 0;
}
