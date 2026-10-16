#pragma once
/** @file LuaRuntimeUiProtected.h
 * @brief Private protected UI marshalling using the existing metadata boundary's outer-RAII ownership pattern.
 */
#include "Horo/Runtime/Ui/UiErrors.h"
#include "LuaRuntimeUiValues.h"

#include <exception>
#include <new>
#include <optional>
#include <type_traits>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail::UiScript {
    /** @brief Signals a caught Lua error whose original error object remains on the Lua stack. */
    struct LuaTransportFailure final {};

    /** @brief Calls a POD/reference-only inner marshaller; C++ owners remain in the caller outside Lua's longjmp frame.
     * @param state Exact synchronously borrowed VM. @param call Inner callable with no nontrivial automatic across Lua API calls.
     * @param arguments Copies the original bounded C-function arguments when decoding; projection needs none.
     * @return Number of projected Lua values. @throws LuaTransportFailure On Lua memory/argument failure.
     * @throws std::bad_alloc On stack admission or an inner native allocation failure, after ordinary C++ unwinding.
     */
    template <class Call> int Protected(lua_State *state, Call call, const bool arguments = false) {
        static_assert(std::is_trivially_destructible_v<Call>, "Inner Lua marshallers must borrow their owners.");

        struct Frame final {
            Call &call;
            std::exception_ptr exception;
        };

        Frame frame{call, {}};

        const int base = lua_gettop(state);
        const int count = arguments ? base : 0;
        if (count > 12 || !lua_checkstack(state, count + LUA_MINSTACK))
            throw std::bad_alloc{};
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
        if (lua_pcall(state, count + 1, LUA_MULTRET, 0) != LUA_OK)
            throw LuaTransportFailure{};
        if (frame.exception)
            std::rethrow_exception(frame.exception);
        return lua_gettop(state) - base;
    }

    /** @brief Reads locked transport through a protected raw metatable lookup, retaining no VM or arbitrary pointer. */
    template <class T> T *Transport(lua_State *state, const int index, const char *name) {
        T *value{};
        Protected(state, [&value, index, name](lua_State *inner) {
            value = static_cast<T *>(luaL_testudata(inner, index, name));
            return 0;
        }, true);
        return value;
    }

    /** @brief Attempts the typed memory failure after native owners unwind; failure selects a nonallocating static fallback. */
    inline std::optional<int> MemoryFailure(lua_State *state) noexcept {
        try {
            return Failure(state, MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
        } catch (...) {
            return std::nullopt;
        }
    }

    /** @brief Unwinds native owners before propagating a Lua error; every C++ exception is contained by this C callback boundary. */
    template <lua_CFunction Function> int Boundary(lua_State *state) noexcept {
        enum class FailureKind {
            Lua,
            Memory,
            Native
        };
        FailureKind failure = FailureKind::Lua;
        try {
            if (lua_gettop(state) > 12)
                return Invalid(state);
            return Function(state);
        } catch (const LuaTransportFailure &) {
            failure = FailureKind::Lua;
        } catch (const std::bad_alloc &) {
            failure = FailureKind::Memory;
        } catch (...) {
            failure = FailureKind::Native;
        }
        // No active C++ catch or nontrivial automatic may remain when Lua performs its C longjmp.
        if (failure == FailureKind::Memory) {
            if (const auto result = MemoryFailure(state); result.has_value())
                return *result;
            return luaL_error(state, "Runtime UI transport memory capacity exceeded.");
        }
        if (failure == FailureKind::Native)
            return luaL_error(state, "Runtime UI transport native failure.");
        return lua_error(state);
    }

    static_assert(std::is_trivially_destructible_v<std::optional<int>>);
    static_assert(std::is_convertible_v<decltype(&Boundary<Invalid>), lua_CFunction>);
    static_assert(noexcept(Boundary<Invalid>(nullptr)));
}  // namespace Horo::Gameplay::Detail::UiScript
