#include "LuaRuntimeUiValues.h"

#include "LuaRuntimeUiProtected.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail::UiScript {
    using namespace Runtime::Ui;

    /** @copydoc PushUnsigned */
    void PushUnsigned(lua_State *state, const std::uint64_t value) {
        std::array<char, 20> text{};
        const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
        lua_pushlstring(state, text.data(), static_cast<std::size_t>(converted.ptr - text.data()));
    }

    /** @copydoc ReadUnsigned */
    bool ReadUnsigned(lua_State *state, const int index, std::uint64_t &value) noexcept {
        if (lua_isinteger(state, index)) {
            const auto integer = lua_tointeger(state, index);
            if (integer < 0)
                return false;
            value = static_cast<std::uint64_t>(integer);
            return true;
        }
        if (lua_type(state, index) != LUA_TSTRING)
            return false;
        std::size_t size{};
        const char *text = lua_tolstring(state, index, &size);
        if (!size || size > 20)
            return false;
        const auto result = std::from_chars(text, text + size, value);
        return result.ec == std::errc{} && result.ptr == text + size;
    }

    /** @copydoc ReadIdentity */
    Result<SerializedUiId> ReadIdentity(lua_State *state, const int index) {
        if (lua_type(state, index) != LUA_TSTRING)
            return Result<SerializedUiId>::Failure(MakeError(UiErrors::IdentityInvalid));
        std::size_t size{};
        const char *text = lua_tolstring(state, index, &size);
        if (size != 32)
            return Result<SerializedUiId>::Failure(MakeError(UiErrors::IdentityInvalid));
        SerializedUiId bytes{};
        for (std::size_t i = 0; i < 16; ++i) {
            unsigned value{};
            if (const auto parsed = std::from_chars(text + 2 * i, text + 2 * i + 2, value, 16);
                parsed.ec != std::errc{} || parsed.ptr != text + 2 * i + 2)
                return Result<SerializedUiId>::Failure(MakeError(UiErrors::IdentityInvalid));
            bytes[i] = static_cast<std::uint8_t>(value);
        }
        return Result<SerializedUiId>::Success(bytes);
    }

    /** @copydoc Failure */
    int Failure(lua_State *state, const Error &error) {
        return Protected(state, [&error](lua_State *inner) {
            lua_pushnil(inner);
            lua_createtable(inner, 0, 2);
            lua_pushstring(inner, error.code.Value().c_str());
            lua_setfield(inner, -2, "code");
            lua_pushstring(inner, error.domain.Value().c_str());
            lua_setfield(inner, -2, "domain");
            return 2;
        });
    }

    /** @copydoc Invalid */
    int Invalid(lua_State *state) {
        return Failure(state, MakeError(UiErrors::CapabilityUnsupported));
    }

    namespace {
        /** @brief Owns only a closed Horo value; finalization never invokes a provider or script callback. */
        template <class T> int Collect(lua_State *state) {
            std::destroy_at(static_cast<T *>(lua_touserdata(state, 1)));
            return 0;
        }

        /** @brief Prepare inert typed storage and a locked metatable before owning a value. */
        template <class T> T *Storage(lua_State *state, const char *name) {
            static_assert(std::is_nothrow_default_constructible_v<T>);
            if (!lua_checkstack(state, 4))
                luaL_error(state, "UI value stack capacity exhausted");
            // A failed protected preparation may have registered only part of this table.
            luaL_newmetatable(state, name);
            lua_pushboolean(state, false);
            lua_setfield(state, -2, "__metatable");
            lua_pushcfunction(state, Collect<T>);
            lua_setfield(state, -2, "__gc");
            auto *value = static_cast<T *>(lua_newuserdatauv(state, sizeof(T), 0));
            std::construct_at(value);
            lua_pushvalue(state, -2);
            lua_setmetatable(state, -2);
            lua_remove(state, -2);
            return value;
        }

        /** @brief Project one borrowed canonical alternative using only POD scratch across possible Lua allocation failures. */
        template <class T> int PushBindingAlternative(lua_State *state, const T &item) {
            if constexpr (std::is_same_v<T, UiBindingNullValue>)
                return 1;
            else if constexpr (std::is_same_v<T, bool>) {
                lua_pushboolean(state, item);
                return 2;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                std::array<char, 21> text{};
                const auto r = std::to_chars(text.data(), text.data() + text.size(), item);
                lua_pushlstring(state, text.data(), static_cast<std::size_t>(r.ptr - text.data()));
                return 2;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                PushUnsigned(state, item);
                return 2;
            } else if constexpr (std::is_same_v<T, double>) {
                lua_pushnumber(state, item);
                return 2;
            } else if constexpr (std::is_same_v<T, std::string>) {
                lua_pushlstring(state, item.data(), item.size());
                return 2;
            } else if constexpr (std::is_same_v<T, UiBindingLocalizedMessage>) {
                lua_pushlstring(state, item.key.data(), item.key.size());
                return 2;
            } else if constexpr (std::is_same_v<T, UiBindingEnumValue> || std::is_same_v<T, UiBindingFlagsValue>) {
                PushUnsigned(state, item.value);
                return 2;
            } else if constexpr (std::is_same_v<T, UiBindingReference>) {
                lua_pushinteger(state, static_cast<int>(item.kind));
                lua_pushlstring(state, item.value.data(), item.value.size());
                return 3;
            } else if constexpr (std::is_same_v<T, UiBindingColor>) {
                lua_pushnumber(state, item.r);
                lua_pushnumber(state, item.g);
                lua_pushnumber(state, item.b);
                lua_pushnumber(state, item.a);
                return 5;
            } else {
                lua_pushnumber(state, item.x);
                lua_pushnumber(state, item.y);
                if constexpr (std::is_same_v<T, UiBindingVector3>) {
                    lua_pushnumber(state, item.z);
                    return 4;
                }
                return 3;
            }
        }

        /** @brief Return closed variant position and primitive components, retaining no native object. */
        int Unpack(lua_State *state) {
            const auto *value = Transport<UiBindingValue>(state, 1, ValueType);
            if (!value)
                return Invalid(state);
            return Protected(state, [value](lua_State *state) {
                lua_pushinteger(state, static_cast<lua_Integer>(value->index()));
                return std::visit([state](const auto &item) {
                    return PushBindingAlternative(state, item);
                }, *value);
            });
        }

        /** @brief Admit only bounded raw primitive text. */
        bool Text(lua_State *state, const int index, std::string &output) {
            if (lua_type(state, index) != LUA_TSTRING)
                return false;
            std::size_t size{};
            const char *value = lua_tolstring(state, index, &size);
            if (size > 4096)
                return false;
            output.assign(value, size);
            return true;
        }

        /** @brief Rejects non-finite or out-of-range values before narrowing into the canonical float representation. */
        bool Float(lua_State *state, const int index, float &output) noexcept {
            if (lua_type(state, index) != LUA_TNUMBER)
                return false;
            const auto value = lua_tonumber(state, index);
            if (!std::isfinite(value) || value < std::numeric_limits<float>::lowest() || value > std::numeric_limits<float>::max())
                return false;
            output = static_cast<float>(value);
            return true;
        }

        /** @brief Decodes primitive binding values without coercion or an allocating Lua API. */
        bool ScalarBindingValue(lua_State *state, const lua_Integer kind, UiBindingValue &value) {
            std::uint64_t integer{};
            switch (kind) {
                case 0:
                    value = UiBindingNullValue{};
                    return true;
                case 1:
                    if (lua_type(state, 3) != LUA_TBOOLEAN)
                        return false;
                    value = static_cast<bool>(lua_toboolean(state, 3));
                    return true;
                case 2: {
                    if (lua_type(state, 3) != LUA_TSTRING)
                        return false;
                    std::size_t size{};
                    const char *text = lua_tolstring(state, 3, &size);
                    std::int64_t signedValue{};
                    if (const auto parsed = std::from_chars(text, text + size, signedValue);
                        !size || parsed.ec != std::errc{} || parsed.ptr != text + size)
                        return false;
                    value = signedValue;
                    return true;
                }
                case 3:
                    if (!ReadUnsigned(state, 3, integer))
                        return false;
                    value = integer;
                    return true;
                case 4:
                    if (lua_type(state, 3) != LUA_TNUMBER || !std::isfinite(lua_tonumber(state, 3)))
                        return false;
                    value = lua_tonumber(state, 3);
                    return true;
                default:
                    return false;
            }
        }

        /** @brief Copies the bounded text and exact unsigned semantic wrapper alternatives. */
        bool SemanticBindingValue(lua_State *state, const lua_Integer kind, UiBindingValue &value) {
            if (kind == 5 || kind == 6) {
                std::string text;
                if (!Text(state, 3, text))
                    return false;
                if (kind == 5)
                    value = std::move(text);
                else
                    value = UiBindingLocalizedMessage{std::move(text)};
                return true;
            }
            std::uint64_t integer{};
            if ((kind != 7 && kind != 8) || !ReadUnsigned(state, 3, integer))
                return false;
            if (kind == 7)
                value = UiBindingEnumValue{integer};
            else
                value = UiBindingFlagsValue{integer};
            return true;
        }

        /** @brief Validates finite structured components and copied stable-reference representation. */
        bool CompositeBindingValue(lua_State *state, const lua_Integer kind, UiBindingValue &value) {
            if (kind >= 9 && kind <= 11) {
                int count = 4;
                if (kind == 9)
                    count = 2;
                else if (kind == 10)
                    count = 3;
                std::array<float, 4> components{};
                for (int index = 0; index < count; ++index)
                    if (!Float(state, 3 + index, components[static_cast<std::size_t>(index)]))
                        return false;
                if (kind == 9)
                    value = UiBindingVector2{components[0], components[1]};
                else if (kind == 10)
                    value = UiBindingVector3{components[0], components[1], components[2]};
                else
                    value = UiBindingColor{components[0], components[1], components[2], components[3]};
                return true;
            }
            std::string text;
            if (kind != 12 || !lua_isinteger(state, 3) || lua_tointeger(state, 3) < 0 ||
                lua_tointeger(state, 3) >= static_cast<int>(UiBindingReferenceKind::Count) || !Text(state, 4, text))
                return false;
            value = UiBindingReference{static_cast<UiBindingReferenceKind>(lua_tointeger(state, 3)), std::move(text)};
            return true;
        }

        /** @brief Selects one canonical family without changing validation or allocation order. */
        bool DecodeBindingValue(lua_State *state, const lua_Integer kind, UiBindingValue &value) {
            if (kind <= 4)
                return ScalarBindingValue(state, kind, value);
            if (kind <= 8)
                return SemanticBindingValue(state, kind, value);
            return CompositeBindingValue(state, kind, value);
        }

        /** @brief Builds the canonical closed vocabulary; owners unwind before any Lua failure is propagated. */
        int BindingValue(lua_State *state) {
            if (!lua_isinteger(state, 2))
                return Invalid(state);
            const auto kind = lua_tointeger(state, 2);
            UiBindingValue value;
            if (const bool decoded = DecodeBindingValue(state, kind, value); !decoded)
                return Invalid(state);
            PushBindingValue(state, value);
            return 1;
        }

        /** @brief Decodes exact nonzero action identities and retains their canonical error identity. */
        Result<UiActionValue> ActionIdentity(lua_State *state, const lua_Integer kind) {
            auto bytes = ReadIdentity(state, 3);
            if (bytes.HasError())
                return Result<UiActionValue>::Failure(bytes.ErrorValue());
            if (bytes.Value() == SerializedUiId{})
                return Result<UiActionValue>::Failure(MakeError(UiErrors::CapabilityUnsupported));
            switch (kind) {
                case 5:
                    return Result<UiActionValue>::Success(UiActionId::Create(bytes.Value()).Value());
                case 6:
                    return Result<UiActionValue>::Success(UiDocumentId::Create(bytes.Value()).Value());
                case 7:
                    return Result<UiActionValue>::Success(UiCanvasId::Create(bytes.Value()).Value());
                default:
                    return Result<UiActionValue>::Success(UiElementId::Create(bytes.Value()).Value());
            }
        }

        /** @brief Closed allocation-free action value transport, including distinct stable identity tags. */
        int ActionValue(lua_State *state) {
            if (!lua_isinteger(state, 2))
                return Invalid(state);
            const auto kind = lua_tointeger(state, 2);
            UiActionValue value;
            std::uint64_t integer{};
            if (kind == 0 && lua_type(state, 3) == LUA_TBOOLEAN)
                value = static_cast<bool>(lua_toboolean(state, 3));
            else if (kind == 1 && lua_isinteger(state, 3))
                value = static_cast<std::int64_t>(lua_tointeger(state, 3));
            else if (kind == 2 && ReadUnsigned(state, 3, integer))
                value = integer;
            else if (kind == 3 && lua_type(state, 3) == LUA_TNUMBER && std::isfinite(lua_tonumber(state, 3)))
                value = lua_tonumber(state, 3);
            else if (kind == 4) {
                std::string text;
                if (!Text(state, 3, text))
                    return Invalid(state);
                auto result = UiActionText::Create(text);
                if (result.HasError())
                    return Failure(state, result.ErrorValue());
                value = result.Value();
            } else if (kind >= 5 && kind <= 8) {
                auto identity = ActionIdentity(state, kind);
                if (identity.HasError())
                    return Failure(state, identity.ErrorValue());
                value = std::move(identity).Value();
            } else
                return Invalid(state);
            return Protected(state, [&value](lua_State *inner) {
                *Storage<UiActionValue>(inner, ActionValueType) = value;
                return 1;
            });
        }
    }  // namespace

    /** @copydoc PushBindingValue */
    void PushBindingValue(lua_State *state, const UiBindingValue &value) {
        Protected(state, [&value](lua_State *inner) {
            *Storage<UiBindingValue>(inner, ValueType) = value;
            luaL_getmetatable(inner, ValueType);
            lua_newtable(inner);
            lua_pushcfunction(inner, Boundary<Unpack>);
            lua_setfield(inner, -2, "unpack");
            lua_setfield(inner, -2, "__index");
            lua_pop(inner, 1);
            return 1;
        });
    }

    /** @copydoc InstallValueFactories */
    void InstallValueFactories(lua_State *state) {
        lua_pushcfunction(state, Boundary<BindingValue>);
        lua_setfield(state, -2, "binding_value");
        lua_pushcfunction(state, Boundary<ActionValue>);
        lua_setfield(state, -2, "action_value");
    }
}  // namespace Horo::Gameplay::Detail::UiScript
