#include "Horo/Gameplay/GameplayVfxContext.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Gameplay::GameplayVfxReceipt>);
static_assert(std::is_trivially_copyable_v<Horo::Gameplay::GameplayVfxSubmission>);
static_assert(std::is_trivially_copyable_v<Horo::Gameplay::GameplayVfxResult>);
