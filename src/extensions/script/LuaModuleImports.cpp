#include "LuaModuleInternal.h"

namespace Horo::Extensions::LuaModuleDetail {
    namespace {
        void Path(lua_State *lua, std::span<const std::string> segments) {
            for (const auto &name : segments) {
                lua_pushlstring(lua, name.data(), name.size());
                lua_newtable(lua);
                lua_pushvalue(lua, -1);
                lua_insert(lua, -3);
                lua_rawset(lua, -4);
                lua_remove(lua, -2);
            }
        }

        void Methods(lua_State *lua, const ScriptModuleImport &imported, std::size_t index) {
            const auto &functions = imported.Backend()->Descriptor().functions;
            for (const auto method : Owner(lua).plans[index]->functionIndices) {
                lua_pushinteger(lua, static_cast<lua_Integer>(index));
                lua_pushinteger(lua, static_cast<lua_Integer>(method));
                lua_pushcclosure(lua, Invoke, 2);
                lua_setfield(lua, -2, functions[method].id.c_str());
            }
        }

        void Constants(lua_State *lua, State &state, const ScriptExportDescriptor &api, const ScriptModuleBindingPlan &plan) {
            for (const auto index : plan.constantIndices) {
                const auto &constant = api.constants.at(index);
                if (!state.PrepareConstant(constant))
                    luaL_error(lua, "Constant projection exceeded its declared bound.");
                state.values.lua = lua;
                state.values.Push(*state.scratchValue);
                lua_setfield(lua, -2, constant.id.c_str());
            }
        }

        void Missing(lua_State *lua, const ScriptModuleImport &imported) {
            lua_pushboolean(lua, 0);
            lua_setfield(lua, -2, "available");
            lua_pushlstring(lua, imported.Unavailable()->code.Value().data(), imported.Unavailable()->code.Value().size());
            lua_setfield(lua, -2, "code");
        }
    }  // namespace

    int BuildImports(lua_State *lua) {
        auto &state = Owner(lua);
        lua_newtable(lua);
        for (std::size_t index = 0; index < state.authority->Imports().size(); ++index) {
            const auto &imported = state.authority->Imports()[index];
            lua_newtable(lua);
            if (imported.Backend()) {
                lua_pushboolean(lua, 1);
                lua_setfield(lua, -2, "available");
                lua_pushvalue(lua, -1);
                Path(lua, state.plans[index]->namespaceSegments);
                Constants(lua, state, imported.Backend()->Descriptor(), *state.plans[index]);
                Methods(lua, imported, index);
                lua_pop(lua, 1);
            } else
                Missing(lua, imported);
            lua_rawseti(lua, -2, static_cast<lua_Integer>(index + 1));
        }
        state.importsRef = luaL_ref(lua, LUA_REGISTRYINDEX);
        state.values.PrepareValueBox(BoxKind::Null, ScriptValue::Null());
        lua_rawgeti(lua, LUA_REGISTRYINDEX, state.horoRef);
        state.values.lua = lua;
        state.values.PushPreparedBox();
        lua_setfield(lua, -2, "null");
        lua_pop(lua, 1);
        state.scratchResult.reset();
        state.scratchValue.reset();
        state.values.lua = state.thread;
        return 0;
    }

    int BindImports(lua_State *lua) {
        bool success = false;
        try {
            BuildImports(lua);
            success = true;
        } catch (...) {
        }
        if (!success)
            return luaL_error(lua, "Import projection allocation or type boundary failed.");
        return 0;
    }

    int Import(lua_State *lua) {
        auto &state = Owner(lua);
        if (!state.Live())
            return luaL_error(lua, "Module authority is revoked.");
        if (lua_gettop(lua) != 1 || lua_type(lua, 1) != LUA_TSTRING)
            return luaL_error(lua, "Import requires one declared alias string.");
        std::size_t length{};
        const char *name = lua_tolstring(lua, 1, &length);
        if (length > 256)
            return luaL_error(lua, "Import alias exceeds its bound.");
        for (std::size_t index = 0; index < state.authority->Imports().size(); ++index) {
            if (state.authority->Imports()[index].Alias() != std::string_view{name, length})
                continue;
            lua_rawgeti(lua, LUA_REGISTRYINDEX, state.importsRef);
            lua_rawgeti(lua, -1, static_cast<lua_Integer>(index + 1));
            return 1;
        }
        return luaL_error(lua, "Import alias was not declared by the actual module plan.");
    }

    bool State::PrepareConstant(const ScriptExportConstantDescriptor &constant) noexcept {
        try {
            auto decoded = DecodeScriptValue(constant.canonicalValue, authority->Limits().values);
            if (decoded.HasError())
                return false;
            scratchValue = std::move(decoded).Value();
            return true;
        } catch (...) {
            return false;
        }
    }
}  // namespace Horo::Extensions::LuaModuleDetail
