#include "LuaModuleInternal.h"

#include <charconv>

namespace Horo::Extensions::LuaModuleDetail {
    namespace {
        ScriptValue ErrorValue(const ScriptError &error) {
            ScriptValue::MapEntries details;
            details.reserve(error.details.size());
            for (const auto &detail : error.details)
                details.emplace_back(ScriptValue{detail.id}, detail.value);
            return ScriptValue::Struct("horo.script.error", {{"domain", ScriptValue{error.domain}},
                                                             {"code", ScriptValue{error.code}},
                                                             {"message", ScriptValue{error.message}},
                                                             {"retryable", ScriptValue{error.retryable}},
                                                             {"cancelled", ScriptValue{error.cancelled}},
                                                             {"details", ScriptValue::Map(std::move(details))}});
        }
    }  // namespace

    void State::PrepareUnsignedText() {
        auto value = values.Unbox(1, BoxKind::Unsigned);
        char digits[21]{};
        const auto parsed = std::to_chars(digits, digits + sizeof(digits), *value.AsUnsignedInteger());
        scratchValue = ScriptValue::String({digits, static_cast<std::size_t>(parsed.ptr - digits)});
    }

    bool State::ProjectOutcome(const Box &box, std::string_view key) {
        auto decoded = DecodeScriptCallResult(box.encoded, authority->Limits().values);
        if (decoded.HasError())
            return false;
        scratchResult = std::move(decoded).Value();
        if (key == "values") {
            scratchValue = ScriptValue::Array(scratchResult->values);
            return true;
        }
        if (key == "error") {
            scratchValue = scratchResult->error ? ErrorValue(*scratchResult->error) : ScriptValue::Null();
            return true;
        }
        return false;
    }

    bool State::PrepareIndex(lua_State *current) noexcept {
        try {
            RequireLive();
            values.lua = current;
            const auto *box = values.GetBox(1);
            if (!box || lua_type(current, 2) != LUA_TSTRING)
                return false;
            std::size_t size{};
            const char *text = lua_tolstring(current, 2, &size);
            const std::string_view key{text, size};
            if (box->kind == BoxKind::Unsigned && key == "decimal") {
                PrepareUnsignedText();
                return true;
            }
            if (box->kind != BoxKind::Outcome)
                return false;
            return ProjectOutcome(*box, key);
        } catch (...) {
            return false;
        }
    }

    int EqualBox(lua_State *lua) {
        auto &state = Owner(lua);
        state.values.lua = lua;
        if (!state.Live())
            return luaL_error(lua, "Module authority is revoked.");
        const auto *a = state.values.GetBox(1);
        const auto *b = state.values.GetBox(2);
        lua_pushboolean(lua, a && b && a->kind == b->kind && a->encoded == b->encoded);
        return 1;
    }

    int IndexBox(lua_State *lua) {
        auto &state = Owner(lua);
        if (!lua_checkstack(lua, static_cast<int>(state.authority->Limits().values.maximumDepth * 4 + 32)))
            return luaL_error(lua, "Value projection stack limit.");
        if (!state.PrepareIndex(lua)) {
            lua_pushnil(lua);
            return 1;
        }
        // This layer owns no C++ values across allocation; scratch storage belongs to the real owner.
        bool success = false;
        try {
            state.values.Push(*state.scratchValue);
            state.scratchValue.reset();
            state.scratchResult.reset();
            success = true;
        } catch (...) {
        }
        if (!success)
            return luaL_error(lua, "Outcome projection exceeded its bound.");
        return 1;
    }

    int PushOutcomeValues(lua_State *lua) {
        auto &state = Owner(lua);
        state.values.lua = lua;
        state.values.PushPreparedBox();
        return 1;
    }

    void State::CaptureOutcome() {
        const auto *box = values.GetBox(-1);
        if (!box || box->kind != BoxKind::Outcome)
            Invalid("A module program must return an actual host call outcome.");
        auto decoded = DecodeScriptCallResult(box->encoded, authority->Limits().values);
        if (decoded.HasError())
            throw Failure{decoded.ErrorValue()};
        result = std::move(decoded).Value();
    }
}  // namespace Horo::Extensions::LuaModuleDetail
