#pragma once

#include "Horo/Gameplay/Behavior.h"

struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Pushes the private bounded Lua projection of the Physics-owned execution capability.
     * The caller sets the returned table in its normal behavior context. Closures retain only
     * revocable shared admission, never a BehaviorContext/Scene/World pointer.
     */
    void PushLuaPhysicsContext(lua_State *state, const BehaviorContext &context);
}  // namespace Horo::Gameplay::Detail
