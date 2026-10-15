#pragma once
/** @file LuaRuntimeUiTransport.h
 * @brief Private locked typed transport; allocation and owner construction occur only inside protected Lua frames.
 */
#include "LuaRuntimeUiProtected.h"

#include <memory>

namespace Horo::Gameplay::Detail::UiScript {
    /** @brief Collect only owned inert transport values, never UI/provider/VM authority. */
    template <class T> int Collect(lua_State *state) {
        std::destroy_at(static_cast<T *>(lua_touserdata(state, 1)));
        return 0;
    }

    /** @brief Compare copied typed evidence without coercion, allocation or exposing owner storage. */
    template <class T> int Equal(lua_State *state) {
        bool equal{};
        if (lua_type(state, 1) == LUA_TUSERDATA && lua_type(state, 2) == LUA_TUSERDATA && lua_rawlen(state, 1) == sizeof(T) &&
            lua_rawlen(state, 2) == sizeof(T) && lua_getmetatable(state, 1) && lua_getmetatable(state, 2)) {
            equal = lua_rawequal(state, -1, -2) &&
                    *static_cast<const T *>(lua_touserdata(state, 1)) == *static_cast<const T *>(lua_touserdata(state, 2));
            lua_pop(state, 2);
        }
        lua_pushboolean(state, equal);
        return 1;
    }

    /** @brief Allocate locked typed transport; no native pointer can be supplied by script. */
    template <class T> void Push(lua_State *state, const char *name, const T &value) {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        if (!lua_checkstack(state, 4))
            luaL_error(state, "UI transport stack capacity exhausted");
        // Protected OOM may leave a registered partial metatable; complete it before owning data.
        luaL_newmetatable(state, name);
        lua_pushboolean(state, false);
        lua_setfield(state, -2, "__metatable");
        lua_pushcfunction(state, Collect<T>);
        lua_setfield(state, -2, "__gc");
        if constexpr (requires(const T &a, const T &b) { a == b; }) {
            lua_pushcfunction(state, Equal<T>);
            lua_setfield(state, -2, "__eq");
        }
        auto *storage = static_cast<T *>(lua_newuserdatauv(state, sizeof(T), 0));
        std::construct_at(storage);
        lua_pushvalue(state, -2);
        lua_setmetatable(state, -2);
        lua_remove(state, -2);
        *storage = value;
    }

    /** @brief Protects allocation around a borrowed result; its C++ owner remains outside the Lua frame. */
    template <class T> int ProjectTransport(lua_State *state, const char *name, const T &value) {
        return Protected(state, [name, &value](lua_State *inner) {
            Push(inner, name, value);
            return 1;
        });
    }

}  // namespace Horo::Gameplay::Detail::UiScript
