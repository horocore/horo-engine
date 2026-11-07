#include "Horo/Extensions/ExtensionErrors.h"
#include "LuaModuleValues.h"

#include <algorithm>
#include <cmath>

namespace Horo::Extensions::LuaModuleDetail {
    [[noreturn]] void Invalid(std::string_view reason) {
        throw Failure{MakeError(ExtensionErrors::ScriptValueTypeMismatch, std::string(reason))};
    }

    void Budget::Charge(std::size_t depth, std::size_t payload) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw Failure{MakeError(ExtensionErrors::ScriptInvocationTimeout)};
        if (depth > limits.maximumDepth || nodes >= std::min(limits.maximumElements, limits.maximumWorkUnits) ||
            payload > limits.maximumBytes - bytes)
            throw Failure{MakeError(ExtensionErrors::ScriptValueCapacityExceeded)};
        ++nodes;
        bytes += payload;
    }

    namespace {
        struct Top final {
            lua_State *lua;
            int top;

            ~Top() {
                lua_settop(lua, top);
            }
        };

        ScriptValue String(lua_State *lua, int index, Budget &budget, std::size_t depth, bool bytes) {
            if (lua_type(lua, index) != LUA_TSTRING)
                Invalid("String/bytes require an actual Lua string, never coercion.");
            std::size_t size{};
            const char *text = lua_tolstring(lua, index, &size);
            if (size > budget.limits.maximumStringBytes && !bytes)
                Invalid("String exceeds its bound.");
            budget.Charge(depth, size);
            if (bytes)
                return ScriptValue::BytesValue(std::as_bytes(std::span{text, size}));
            return ScriptValue::String({text, size});
        }

        ScriptValue Boolean(lua_State *lua, int index) {
            if (lua_type(lua, index) != LUA_TBOOLEAN)
                Invalid("Boolean required.");
            return ScriptValue{lua_toboolean(lua, index) != 0};
        }

        ScriptValue Number(lua_State *lua, int index) {
            if (lua_type(lua, index) != LUA_TNUMBER)
                Invalid("Finite number required.");
            const auto number = lua_tonumber(lua, index);
            if (!std::isfinite(number))
                Invalid("Finite number required.");
            return ScriptValue{static_cast<double>(number)};
        }

        std::int64_t Integer(lua_State *lua, int index) {
            if (!lua_isinteger(lua, index))
                Invalid("An exact signed integer is required.");
            return static_cast<std::int64_t>(lua_tointeger(lua, index));
        }
    }  // namespace

    void Values::Plain(int index) const {
        if (lua_type(lua, index) != LUA_TTABLE)
            Invalid("A plain raw table is required.");
        if (lua_getmetatable(lua, index)) {
            lua_pop(lua, 1);
            Invalid("Metatables cannot cross the data boundary.");
        }
    }

    const Box *Values::GetBox(int index) const {
        if (lua_type(lua, index) != LUA_TUSERDATA || lua_rawlen(lua, index) != sizeof(Box *))
            return nullptr;
        if (!lua_getmetatable(lua, index))
            return nullptr;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, metatable);
        const bool same = lua_rawequal(lua, -1, -2);
        lua_pop(lua, 2);
        return same ? *static_cast<Box **>(lua_touserdata(lua, index)) : nullptr;
    }

    ScriptValue Values::Unbox(int index, BoxKind kind) const {
        auto *box = GetBox(index);
        if (!box || box->kind != kind)
            Invalid("An exact host-owned typed value is required.");
        auto decoded = DecodeScriptValue(box->encoded, authority->Limits().values);
        if (decoded.HasError())
            throw Failure{decoded.ErrorValue()};
        return std::move(decoded).Value();
    }

    std::size_t Values::Length(int index, Budget &budget) const {
        index = lua_absindex(lua, index);
        Plain(index);
        const auto length = lua_rawlen(lua, index);
        if (length > budget.limits.maximumElements)
            Invalid("Table exceeds the finite bound.");
        Top restore{lua, lua_gettop(lua)};
        std::size_t count{};
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
            if (!lua_isinteger(lua, -2))
                Invalid("Arrays cannot contain named properties.");
            const auto key = lua_tointeger(lua, -2);
            if (key < 1 || static_cast<std::uint64_t>(key) > length)
                Invalid("Array keys must be dense and one-based.");
            budget.Charge(0);
            ++count;
            lua_pop(lua, 1);
        }
        if (count != length)
            Invalid("Sparse arrays are not admitted.");
        return length;
    }

    ScriptValue Values::Primitive(int index, ScriptExportPrimitiveKind type, Budget &budget, std::size_t depth) {
        using P = ScriptExportPrimitiveKind;
        switch (type) {
            case P::Boolean:
                return Boolean(lua, index);
            case P::SignedInteger:
                return ScriptValue{Integer(lua, index)};
            case P::UnsignedInteger:
                return Unbox(index, BoxKind::Unsigned);
            case P::Number:
                return Number(lua, index);
            case P::String:
                return String(lua, index, budget, depth, false);
            case P::Bytes:
                return String(lua, index, budget, depth, true);
            case P::Handle: {
                auto handle = Unbox(index, BoxKind::Handle);
                auto valid = authority->ValidateHandle(*handle.AsHandle());
                if (valid.HasError())
                    throw Failure{valid.ErrorValue()};
                return handle;
            }
            default:
                Invalid("Unsupported primitive type.");
        }
    }

    ScriptValue Values::Read(int index, const ScriptExportTypeReference &type, const ScriptExportDescriptor &api, Budget &budget,
                             std::size_t depth) {
        budget.Charge(depth);
        const auto *box = GetBox(index);
        if (box && box->kind == BoxKind::Null) {
            if (type.nullability != ScriptExportNullability::Nullable)
                Invalid("Null is not allowed here.");
            return ScriptValue::Null();
        }
        if (type.primitive != ScriptExportPrimitiveKind::Count)
            return Primitive(index, type.primitive, budget, depth);
        const auto found = std::ranges::find(api.types, type.namedType, &ScriptExportTypeDescriptor::id);
        if (found == api.types.end())
            Invalid("Named type is absent from the admitted generation.");
        return Named(index, *found, api, budget, depth);
    }

    ScriptValue Values::Named(int index, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api, Budget &budget,
                              std::size_t depth) {
        switch (type.kind) {
            case ScriptExportNamedTypeKind::Enum:
                return ScriptValue{ScriptEnumValue{type.id, Integer(lua, index)}};
            case ScriptExportNamedTypeKind::Array:
                return Array(index, type, api, budget, depth);
            case ScriptExportNamedTypeKind::Map:
                return Map(index, type, api, budget, depth);
            case ScriptExportNamedTypeKind::Struct:
                return Struct(index, type, api, budget, depth);
            default:
                Invalid("Unsupported named type.");
        }
    }

    ScriptValue Values::Array(int index, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api, Budget &budget,
                              std::size_t depth) {
        index = lua_absindex(lua, index);
        const auto size = Length(index, budget);
        if (size > std::min(type.maximumElements, budget.limits.maximumArrayElements))
            Invalid("Array exceeds its declared bound.");
        Top restore{lua, lua_gettop(lua)};
        ScriptValue::ArrayElements result;
        result.reserve(size);
        for (std::size_t i = 1; i <= size; ++i) {
            lua_rawgeti(lua, index, static_cast<lua_Integer>(i));
            result.push_back(Read(-1, type.elementType, api, budget, depth + 1));
            lua_pop(lua, 1);
        }
        return ScriptValue::Array(std::move(result));
    }

    ScriptValue Values::Map(int index, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api, Budget &budget,
                            std::size_t depth) {
        index = lua_absindex(lua, index);
        const auto size = Length(index, budget);
        if (size > std::min(type.maximumElements, budget.limits.maximumMapEntries))
            Invalid("Map exceeds its declared bound.");
        Top restore{lua, lua_gettop(lua)};
        ScriptValue::MapEntries result;
        result.reserve(size);
        for (std::size_t i = 1; i <= size; ++i) {
            lua_rawgeti(lua, index, static_cast<lua_Integer>(i));
            if (Length(-1, budget) != 2)
                Invalid("Map entries require exactly {key,value}.");
            lua_rawgeti(lua, -1, 1);
            auto key = Read(-1, type.keyType, api, budget, depth + 1);
            lua_pop(lua, 1);
            lua_rawgeti(lua, -1, 2);
            auto value = Read(-1, type.valueType, api, budget, depth + 1);
            lua_pop(lua, 2);
            result.emplace_back(std::move(key), std::move(value));
        }
        return ScriptValue::Map(std::move(result));
    }

    ScriptValue Values::Struct(int index, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api, Budget &budget,
                               std::size_t depth) {
        index = lua_absindex(lua, index);
        Plain(index);
        Top restore{lua, lua_gettop(lua)};
        ScriptValue::StructFields fields;
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
            if (fields.size() >= budget.limits.maximumStructFields || lua_type(lua, -2) != LUA_TSTRING)
                Invalid("Struct requires bounded own string fields.");
            std::size_t size{};
            const char *key = lua_tolstring(lua, -2, &size);
            budget.Charge(depth, size);
            const auto found = std::ranges::find(type.fields, std::string_view{key, size}, &ScriptExportFieldDescriptor::id);
            if (found == type.fields.end())
                Invalid("Unknown struct field.");
            fields.emplace_back(std::string{key, size}, Read(-1, found->type, api, budget, depth + 1));
            lua_pop(lua, 1);
        }
        return ScriptValue::Struct(type.id, std::move(fields));
    }
}  // namespace Horo::Extensions::LuaModuleDetail
