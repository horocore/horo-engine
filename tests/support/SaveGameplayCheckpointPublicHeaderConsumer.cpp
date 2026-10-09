#include "Horo/Runtime/Save/SaveGameplayCheckpoint.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Runtime::GameplayCheckpoint>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::GameplayCheckpointController>);

int main() {
    Horo::Runtime::GameplayCheckpointController controller;
    return controller.Active() == nullptr ? 0 : 1;
}
