#include "Horo/Gameplay/GameplayVfxContext.h"
#include "Horo/Vfx/VfxWorld.h"

namespace {
    /** @brief Links the opaque client API without inventing a trusted Scene scope. */
    bool ProbeClient(const Horo::Gameplay::GameplayVfxContext *context) {
        return context && context->IsUsable();
    }
}  // namespace

int main() {
    using Probe = bool (*)(const Horo::Gameplay::GameplayVfxContext *);
    Probe volatile probe = &ProbeClient;
    if (probe(nullptr))
        return 1;
    const auto decoded = Horo::Gameplay::DecodeGameplayVfxBindings("null");
    if (!decoded.HasError())
        return 2;
    // Exercise the complete neutral static-link closure, without native rendering.
    const auto world = Horo::Vfx::VfxWorld::PrepareSpawns({}, {}, {}, {});
    return world.HasError() ? 0 : 3;
}
