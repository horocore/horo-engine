#pragma once
#include <exception>
#include <type_traits>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail::DecisionScript {
    /** @brief Keeps C++ owners outside Lua's longjmp region, including allocator and metadata failures. */
    template <typename Call> int ProtectedStatus(lua_State *state, Call call, bool arguments = false) {
        static_assert(std::is_trivially_destructible_v<Call>);

        struct Frame {
            Call &call;
            std::exception_ptr exception;
        };

        Frame frame{call, {}};

        const int count = arguments ? lua_gettop(state) : 0;
        if (count > 12)
            return LUA_ERRRUN;
        if (!lua_checkstack(state, count + LUA_MINSTACK))
            return LUA_ERRMEM;
        lua_pushcfunction(state, +[](lua_State *inner) noexcept -> int {
            auto &frame = *static_cast<Frame *>(lua_touserdata(inner, -1));
            lua_pop(inner, 1);
            try {
                return frame.call(inner);
            } catch (...) {
                frame.exception = std::current_exception();
                return 0;
            }
        });
        for (int index = 1; index <= count; ++index)
            lua_pushvalue(state, index);
        lua_pushlightuserdata(state, &frame);
        const int status = lua_pcall(state, count + 1, LUA_MULTRET, 0);
        if (frame.exception)
            std::rethrow_exception(frame.exception);
        return status;
    }

    /** @brief Runs a protected operation whose caller needs only success or failure. */
    template <typename Call> bool Protected(lua_State *state, Call call, bool arguments = false) {
        return ProtectedStatus(state, call, arguments) == LUA_OK;
    }
}  // namespace Horo::Gameplay::Detail::DecisionScript
