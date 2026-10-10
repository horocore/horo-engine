#include "Horo/Runtime/Save/SaveSlotLifecycle.h"

#include <type_traits>

static_assert(std::is_move_constructible_v<Horo::Runtime::SaveSlotLifecycle>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveSlotLifecycle>);

int main() {
    const Horo::Runtime::SaveSlotLifecyclePolicy policy;
    return policy.capabilities == 0 && policy.signature == Horo::Runtime::SaveSignaturePolicy::Required ? 0 : 1;
}
