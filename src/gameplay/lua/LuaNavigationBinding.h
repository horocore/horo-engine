#pragma once
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/Behavior.h"
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Push owned navigation closures; VM retirement cancels the copied lifetime token. */
    void PushLuaNavigationContext(lua_State *state, const BehaviorContext &context, CancellationToken lifetime);
}  // namespace Horo::Gameplay::Detail
