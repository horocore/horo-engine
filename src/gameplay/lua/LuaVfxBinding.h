#pragma once

#include <memory>

struct lua_State;

namespace Horo::Gameplay {
    class GameplayVfxContext;
}

namespace Horo::Gameplay::Detail {
    /** @brief Pushes the real entity-scoped VFX client, retaining only revocable authority, never a callback or World borrow. */
    void PushLuaVfxContext(lua_State *state, const std::shared_ptr<const GameplayVfxContext> &context);
}  // namespace Horo::Gameplay::Detail
