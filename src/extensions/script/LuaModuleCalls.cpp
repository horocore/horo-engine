#include "Horo/Extensions/ExtensionErrors.h"
#include "LuaModuleInternal.h"

#include <charconv>

namespace Horo::Extensions::LuaModuleDetail {
    bool State::RecordError(const Error &error) noexcept {
        try {
            auto converted = MakeScriptError(error, false, authority->Limits().values);
            if (converted.HasError())
                return false;
            scratchResult = ScriptCallResult::Failure(std::move(converted).Value());
            values.PrepareOutcome(*scratchResult);
            scratchResult.reset();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool State::PrepareCall(lua_State *current, std::string_view imported, std::string_view method) noexcept {
        try {
            RequireLive();
            values.lua = current;
            const auto count = lua_gettop(current);
            auto selected = FindScriptModuleFunction(authority->Imports(), imported, method, static_cast<std::size_t>(count));
            if (selected.HasError())
                return RecordError(selected.ErrorValue());
            const auto &api = *selected.Value().api;
            const auto &function = *selected.Value().function;
            Budget budget{authority->Limits().values, deadline};
            std::vector<ScriptValue> arguments;
            arguments.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index)
                arguments.push_back(values.Read(index + 1, function.parameters[static_cast<std::size_t>(index)].type, api, budget));
            auto call = authority->Invoke(selected.Value().imported->Alias(), function.id, std::move(arguments));
            if (call.HasError())
                return RecordError(call.ErrorValue());
            if (function.invocation == ScriptExportInvocationMode::Asynchronous) {
                pending = std::move(call).Value();
                pendingOrigin = authority;
                awaiting = true;
                return true;
            }
            auto snapshot = call.Value().Snapshot();
            if (!snapshot || !snapshot->terminalResult)
                Invalid("Synchronous call has no actual terminal outcome.");
            scratchResult = std::move(*snapshot->terminalResult);
            values.PrepareOutcome(*scratchResult);
            scratchResult.reset();
            if (auto drained = authority->Poll(); drained.HasError())
                return RecordError(drained.ErrorValue());
            return true;
        } catch (const Failure &failure) {
            return RecordError(failure.error);
        } catch (...) {
            return false;
        }
    }

    int Continue(lua_State *lua, int, lua_KContext) {
        if (!Owner(lua).Live())
            return luaL_error(lua, "Module authority is revoked.");
        return 1;
    }

    int Invoke(lua_State *lua) {
        auto &state = Owner(lua);
        if (!lua_checkstack(lua, static_cast<int>(state.authority->Limits().values.maximumDepth * 4 + 32)))
            return luaL_error(lua, "Value projection stack limit.");
        if (lua_type(lua, lua_upvalueindex(1)) != LUA_TSTRING || lua_type(lua, lua_upvalueindex(2)) != LUA_TSTRING)
            return luaL_error(lua, "Host call identity is invalid.");
        std::size_t aliasSize{};
        std::size_t methodSize{};
        const char *alias = lua_tolstring(lua, lua_upvalueindex(1), &aliasSize);
        if (const char *method = lua_tolstring(lua, lua_upvalueindex(2), &methodSize);
            !state.PrepareCall(lua, {alias, aliasSize}, {method, methodSize}))
            return luaL_error(lua, "Host call failed its declared boundary.");
        if (state.awaiting)
            return lua_yieldk(lua, 0, 0, Continue);
        state.values.PushPreparedBox();
        return 1;
    }

    bool State::PrepareUnsigned(lua_State *current) noexcept {
        try {
            RequireLive();
            if (lua_gettop(current) != 1 || lua_type(current, 1) != LUA_TSTRING)
                Invalid("u64 requires one exact decimal string.");
            std::size_t size{};
            const char *text = lua_tolstring(current, 1, &size);
            if (size == 0 || size > 20)
                Invalid("u64 decimal is out of range.");
            std::uint64_t value{};
            if (const auto parsed = std::from_chars(text, text + size, value); parsed.ec != std::errc{} || parsed.ptr != text + size)
                Invalid("u64 decimal is out of range.");
            scratchValue = ScriptValue{value};
            values.PrepareValueBox(BoxKind::Unsigned, *scratchValue);
            return true;
        } catch (...) {
            return false;
        }
    }

    int Unsigned(lua_State *lua) {
        auto &state = Owner(lua);
        if (!state.PrepareUnsigned(lua))
            return luaL_error(lua, "u64 requires a decimal uint64 in range.");
        state.values.lua = lua;
        state.values.PushPreparedBox();
        return 1;
    }
}  // namespace Horo::Extensions::LuaModuleDetail
