#include "Horo/Extensions/ExtensionErrors.h"
#include "LuaModuleValues.h"

#include <utility>

namespace Horo::Extensions::LuaModuleDetail {
    Box::~Box() {
        if (ledger && cost) {
            ledger->bytes -= cost;
            --ledger->values;
        }
    }

    int CollectBox(lua_State *lua) {
        if (auto **box = static_cast<Box **>(lua_touserdata(lua, 1)); box) {
            // No Lua API call may longjmp across the reclaimed C++ owner.
            const std::unique_ptr<Box> owner{std::exchange(*box, nullptr)};
        }
        return 0;
    }

    int BoxName(lua_State *lua) {
        lua_pushliteral(lua, "horo.typed-value");
        return 1;
    }

    void Values::PrepareBox(BoxKind kind, std::vector<std::byte> encoded) {
        const auto cost = sizeof(Box) + encoded.capacity();
        if (ledger->values >= ledger->maximumValues || cost > ledger->maximumBytes - ledger->bytes)
            throw Failure{MakeError(ExtensionErrors::ScriptValueCapacityExceeded)};
        auto box = std::make_unique<Box>();
        box->kind = kind;
        box->encoded = std::move(encoded);
        box->ledger = ledger;
        box->cost = cost;
        ++ledger->values;
        ledger->bytes += cost;
        preparedBox = std::move(box);
    }

    void Values::PrepareValueBox(BoxKind kind, const ScriptValue &value) {
        auto encoded = EncodeScriptValue(value, authority->Limits().values);
        if (encoded.HasError())
            throw Failure{encoded.ErrorValue()};
        PrepareBox(kind, std::move(encoded).Value());
    }

    void Values::PushPreparedBox() {
        auto **box = static_cast<Box **>(lua_newuserdatauv(lua, sizeof(Box *), 0));
        *box = nullptr;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, metatable);
        lua_setmetatable(lua, -2);
        *box = preparedBox.release();
    }

    namespace {
        void WriteArray(Values &values, std::span<const ScriptValue> source, std::size_t depth) {
            lua_createtable(values.lua, static_cast<int>(source.size()), 0);
            lua_Integer index = 1;
            for (const auto &child : source) {
                values.Push(child, depth + 1);
                lua_rawseti(values.lua, -2, index++);
            }
        }

        void WriteMap(Values &values, std::span<const ScriptValue::MapEntry> source, std::size_t depth) {
            lua_createtable(values.lua, static_cast<int>(source.size()), 0);
            lua_Integer index = 1;
            for (const auto &[key, item] : source) {
                lua_createtable(values.lua, 2, 0);
                values.Push(key, depth + 1);
                lua_rawseti(values.lua, -2, 1);
                values.Push(item, depth + 1);
                lua_rawseti(values.lua, -2, 2);
                lua_rawseti(values.lua, -2, index++);
            }
        }

        void WriteStruct(Values &values, std::span<const ScriptValue::StructField> source, std::size_t depth) {
            lua_createtable(values.lua, 0, static_cast<int>(source.size()));
            for (const auto &[key, item] : source) {
                lua_pushlstring(values.lua, key.data(), key.size());
                values.Push(item, depth + 1);
                lua_rawset(values.lua, -3);
            }
        }
    }  // namespace

    void Values::PushCollection(const ScriptValue &value, std::size_t depth) {
        switch (value.GetKind()) {
            case ScriptValue::Kind::Array:
                WriteArray(*this, value.AsArray(), depth);
                return;
            case ScriptValue::Kind::Map:
                WriteMap(*this, value.AsMap(), depth);
                return;
            case ScriptValue::Kind::Struct:
                WriteStruct(*this, value.AsStruct(), depth);
                return;
            default:
                Invalid("Unsupported Lua output value kind.");
        }
    }

    bool Values::PushTyped(const ScriptValue &value) {
        switch (value.GetKind()) {
            case ScriptValue::Kind::Null:
                PrepareValueBox(BoxKind::Null, value);
                break;
            case ScriptValue::Kind::UnsignedInteger:
                PrepareValueBox(BoxKind::Unsigned, value);
                break;
            case ScriptValue::Kind::Handle:
                PrepareValueBox(BoxKind::Handle, value);
                break;
            default:
                return false;
        }
        PushPreparedBox();
        return true;
    }

    void Values::Push(const ScriptValue &value, std::size_t depth) {
        if (depth > authority->Limits().values.maximumDepth)
            Invalid("Output value exceeds depth bound.");
        if (PushTyped(value))
            return;
        using K = ScriptValue::Kind;
        switch (value.GetKind()) {
            case K::Boolean:
                lua_pushboolean(lua, *value.AsBoolean());
                return;
            case K::SignedInteger:
                lua_pushinteger(lua, static_cast<lua_Integer>(*value.AsSignedInteger()));
                return;

            case K::Number:
                lua_pushnumber(lua, *value.AsNumber());
                return;
            case K::String:
                lua_pushlstring(lua, value.AsString().data(), value.AsString().size());
                return;
            case K::Bytes:
                lua_pushlstring(lua, reinterpret_cast<const char *>(value.AsBytes().data()), value.AsBytes().size());
                return;

            case K::Enum:
                lua_pushinteger(lua, static_cast<lua_Integer>(value.AsEnum()->value));
                return;
            default:
                PushCollection(value, depth);
                return;
        }
    }

    void Values::PrepareOutcome(const ScriptCallResult &result) {
        // Encoding finishes before a Lua allocation: no codec temporary crosses Lua's longjmp boundary.
        auto encoded = EncodeScriptCallResult(result, authority->Limits().values);
        if (encoded.HasError())
            throw Failure{encoded.ErrorValue()};
        PrepareBox(BoxKind::Outcome, std::move(encoded).Value());
        // Caller invokes PushPreparedBox only after this C++ ownership frame has returned.
    }
}  // namespace Horo::Extensions::LuaModuleDetail
