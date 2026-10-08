#include "Horo/Runtime/Save/SaveEventTriggers.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveEventTriggers>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::SaveTriggerEvent>);

template <typename T>
concept AllowsUnvalidatedConstruction =
    requires(const Horo::Runtime::CookedSaveProjectPolicy &policy, const Horo::Runtime::SaveTriggerHostState &host,
             Horo::Runtime::SaveOperationArbiter &arbiter,
             Horo::Runtime::SaveSafePointCoordinator &safePoints) { T{policy, host, arbiter, safePoints, {}, {}}; };
static_assert(!AllowsUnvalidatedConstruction<Horo::Runtime::SaveEventTriggers>);

int main() {
    const Horo::Runtime::SaveTriggerReceipt receipt;
    return Horo::Runtime::DecideSaveTransition(receipt) == Horo::Runtime::SaveTransitionDecision::NotApplicable ? 0 : 1;
}
