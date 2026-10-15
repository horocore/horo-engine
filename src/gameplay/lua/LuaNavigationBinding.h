#pragma once
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/Behavior.h"
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Push GC-safe owned navigation closures from outer protected-frame borrows.
     * @param state Explicit VM with the caller owning its protected Lua error boundary.
     * @param context Outer-owned exact Navigation scope, or null for typed missing access.
     * @param lifetime VM-owned token created outside the inner Lua C frame; retirement cancels it.
     */
    void PushLuaNavigationContext(lua_State *state, const std::shared_ptr<const GameplayNavigationContext> &context,
                                  const CancellationToken &lifetime);
}  // namespace Horo::Gameplay::Detail
