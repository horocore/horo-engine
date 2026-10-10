#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveAutosaveScheduler>);
static_assert(std::is_aggregate_v<Horo::Runtime::SaveAutosaveAdmission>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::SaveAutosaveClockSample>);

using AutosaveCommit = Horo::Result<std::optional<Horo::Runtime::SaveAutosaveCapture>> (Horo::Runtime::SaveAutosaveScheduler::*)(
    Horo::Runtime::RuntimePhase, Horo::Runtime::SaveRuntimeGeneration, Horo::Runtime::SaveAutosaveAdmission,
    const Horo::Runtime::RuntimeSaveCaptureProvenance &, Horo::Runtime::SaveParticipantRegistrySnapshot,
    const Horo::Runtime::RuntimeSaveCaptureLimits &);
static_assert(std::is_same_v<decltype(&Horo::Runtime::SaveAutosaveScheduler::CommitAtSafePoint), AutosaveCommit>);

template <typename T>
concept AllowsUnvalidatedConstruction =
    requires(const Horo::Runtime::SaveAutosavePolicy &policy, const Horo::Runtime::SaveAutosaveClockSample &initial,
             Horo::Runtime::SaveOperationArbiter &arbiter,
             Horo::Runtime::SaveCaptureBarrier &barrier) { T{policy, initial, arbiter, barrier, {}}; };
static_assert(!AllowsUnvalidatedConstruction<Horo::Runtime::SaveAutosaveScheduler>);
static_assert(!std::is_constructible_v<Horo::Runtime::SaveAutosaveScheduler, const Horo::Runtime::SaveAutosavePolicy &,
                                       const Horo::Runtime::SaveAutosaveClockSample &, Horo::Runtime::SaveOperationArbiter &,
                                       Horo::Runtime::SaveCaptureBarrier &>);

int main() {
    const Horo::Runtime::SaveArbiterRetrySnapshot retry;
    if (retry.completedRetries != 0 || retry.lastError)
        return 1;
    const Horo::Runtime::SaveAutosaveSchedulerSnapshot snapshot;
    return snapshot.pending || snapshot.operation != 0 ? 1 : 0;
}
