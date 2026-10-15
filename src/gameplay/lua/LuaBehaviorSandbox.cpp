#include "LuaBehaviorSandbox.h"

#include <initializer_list>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        /** @brief Preserve the inert horo.behavior declaration table without registration side effects. */
        int IdentityBehavior(lua_State *state) {
            luaL_checktype(state, 1, LUA_TTABLE);
            lua_settop(state, 1);
            return 1;
        }

    }  // namespace

    /** @copydoc OpenLuaBehaviorSandbox */
    void OpenLuaBehaviorSandbox(lua_State *state) {
        luaL_requiref(state, "_G", luaopen_base, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);
        for (const char *name : {"dofile", "loadfile", "collectgarbage", "io", "os", "package", "debug"}) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        lua_newtable(state);
        lua_pushcfunction(state, IdentityBehavior);
        lua_setfield(state, -2, "behavior");
        lua_setglobal(state, "horo");
    }

}  // namespace Horo::Gameplay::Detail
