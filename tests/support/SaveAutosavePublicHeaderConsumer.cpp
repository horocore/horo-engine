#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveAutosaveScheduler>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::SaveAutosaveClockSample>);

int main() {
    const Horo::Runtime::SaveAutosaveSchedulerSnapshot snapshot;
    return snapshot.pending || snapshot.operation != 0 ? 1 : 0;
}
