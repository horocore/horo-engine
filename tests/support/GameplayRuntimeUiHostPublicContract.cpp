#include "Horo/Gameplay/GameModuleHost.h"

#include <type_traits>

using namespace Horo::Gameplay;
static_assert(GameplaySdkBoundaryVersion == 17);
static_assert(!std::is_default_constructible_v<GameModuleHostOptions>);
static_assert(!std::is_aggregate_v<GameModuleHostOptions>);

/** @brief Preserves legacy brace-call resolution while the typed options constructor is the sole composition authority. */
void VerifyRuntimeUiHostConstructorContract() {
    GameModuleHost defaults{};
    GameModuleHost capabilities{{}};
    GameModuleHost legacyPhysics{{}, {}};
    GameModuleHost legacyNavigation{{}, {}, {}};
    GameModuleHost explicitServices{GameModuleHostOptions{{}, {}}};
}
