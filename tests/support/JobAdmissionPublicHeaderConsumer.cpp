#include "Horo/Foundation/JobSystem.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(&Horo::JobSystem::SubmitContext), Horo::Result<Horo::JobHandle> (Horo::JobSystem::*)(
                                                                            const Horo::JobDescriptor &, Horo::ContextJobFunction) const>);

int main() {
    Horo::JobSystemConfig config{.workerCount = 0, .maxQueuedJobs = 1};
    config.priorityQueues[2] = {.capacity = 0, .overloadPolicy = Horo::JobOverloadPolicy::Shed};
    Horo::JobSystem jobs{config};
    config.priorityQueues[2] = {.capacity = 1, .overloadPolicy = Horo::JobOverloadPolicy::Reject};
    const auto noWork = [](const Horo::CancellationToken &) {
        // Intentionally no work: this consumer verifies typed admission without executing a callback.
    };
    const auto required = jobs.Submit({.priority = Horo::JobPriority::Background}, noWork);
    const auto optional = jobs.Submit({.priority = Horo::JobPriority::Background, .requirement = Horo::JobRequirement::Optional}, noWork);
    const auto pressure = jobs.AdmissionSnapshot();
    return required.HasError() && required.ErrorValue().code.Value() == "job.queue_full" && optional.HasError() &&
                   optional.ErrorValue().code.Value() == "job.queue_shed" && pressure.rejected[2] == 2 && pressure.shed[2] == 1
               ? 0
               : 1;
}
