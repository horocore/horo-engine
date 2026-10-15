#pragma once
/** @file LuaBehaviorSandbox.h
 * @brief Target-private standard-library admission for the existing behavior VM sandbox.
 */
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Install the existing restricted library set and inert behavior declaration function.
     * @param state Explicit compiler/runtime-owned VM; caller owns its Lua allocation/error boundary.
     */
    void OpenLuaBehaviorSandbox(lua_State *state);
}  // namespace Horo::Gameplay::Detail
