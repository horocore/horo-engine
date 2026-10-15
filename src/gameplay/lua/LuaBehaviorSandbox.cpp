#include "LuaBehaviorSandbox.h"

#include <cstdlib>
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

    /** @copydoc BudgetAllocate */
    void *BudgetAllocate(void *userData, void *pointer, const std::size_t oldSize, const std::size_t newSize) {
        auto &budget = *static_cast<LuaBudget *>(userData);
        // For a fresh Lua allocation oldSize is an object-type tag, not an allocation size.
        const std::size_t accountedOldSize = pointer != nullptr ? oldSize : 0;
        if (newSize == 0) {
            budget.used = accountedOldSize > budget.used ? 0 : budget.used - accountedOldSize;
            std::free(pointer);  // NOSONAR(cpp:S5025) Lua's allocator ABI is the C realloc/free contract.
            return nullptr;
        }
        const std::size_t retained = accountedOldSize > budget.used ? 0 : budget.used - accountedOldSize;
        if (newSize > budget.maximum || retained > budget.maximum - newSize)
            return nullptr;
        void *resized = std::realloc(pointer, newSize);  // NOSONAR(cpp:S5025) Preserves Lua realloc semantics and performance.
        if (resized != nullptr)
            budget.used = retained + newSize;
        return resized;
    }

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
