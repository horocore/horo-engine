#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <type_traits>
static_assert(!std::is_copy_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);

#include "Horo/Runtime/Scene/RuntimeSceneCellLayers.h"
static_assert(!std::is_copy_constructible_v<Horo::Runtime::RuntimeSceneCellLayers>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellLayers>);
