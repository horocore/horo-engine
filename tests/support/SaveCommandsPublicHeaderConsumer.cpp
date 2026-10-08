#include "Horo/Runtime/Save/SaveCommands.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveCommands>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::SaveCommandTarget>);

int main() {
    const Horo::Runtime::SaveCommandSubmission submission;
    return submission.operation.Snapshot().has_value() ? 1 : 0;
}
