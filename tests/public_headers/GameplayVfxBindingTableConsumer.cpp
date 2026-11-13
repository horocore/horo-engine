#include "Horo/Gameplay/GameplayVfxBindingTable.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Gameplay::GameplayVfxEventPayload>);
static_assert(std::is_trivially_copyable_v<Horo::Gameplay::GameplayVfxLookup>);
static_assert(std::is_trivially_copyable_v<Horo::Gameplay::GameplayVfxResolveOutcome>);
