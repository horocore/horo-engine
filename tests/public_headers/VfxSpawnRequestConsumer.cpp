#include "Horo/Vfx/VfxSpawnRequest.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Vfx::VfxSpawnRequest>);
static_assert(std::is_trivially_copyable_v<Horo::Vfx::VfxSpawnOutcome>);
