#include "Horo/Vfx/VfxSpawnPreparation.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Vfx::CpuParticleAdvanceOutcome>);
