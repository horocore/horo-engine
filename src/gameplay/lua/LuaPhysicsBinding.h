#pragma once

#include "Horo/Gameplay/Behavior.h"

struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Pushes the private bounded Lua projection of the Physics-owned execution capability.
     * The caller retains the shared owner outside its protected Lua context construction and sets the returned table. Closures retain only
     * revocable shared admission, never a BehaviorContext/Scene/World pointer.
     */
    void PushLuaPhysicsContext(lua_State *state, const std::shared_ptr<const GameplayPhysicsContext> &physics);
}  // namespace Horo::Gameplay::Detail
