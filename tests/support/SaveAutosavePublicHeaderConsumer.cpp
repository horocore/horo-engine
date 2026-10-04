#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveAutosaveScheduler>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::SaveAutosaveClockSample>);

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
    const Horo::Runtime::SaveAutosaveSchedulerSnapshot snapshot;
    return snapshot.pending || snapshot.operation != 0 ? 1 : 0;
}
