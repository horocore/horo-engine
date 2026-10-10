#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/AITaskScheduler.h"

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
    if (scheduler.Value()->EvaluateAtDecision(1).HasError() || scheduler.Value()->CommitAtIntentDispatch(1).HasError())
        return 4;
    scheduler.Value()->Shutdown();
    return 0;
}
