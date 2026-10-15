#include "Horo/Physics/CharacterErrors.h"
#include "LuaCharacterBinding.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>
extern "C" {
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        /** @brief Reads actual table storage without invoking authored lookup metamethods. */
        void Field(lua_State *state, int index, const char *name) {
            index = lua_absindex(state, index);
            lua_pushstring(state, name);
            lua_rawget(state, index);
        }

        /** @brief Bounds schema work and rejects unsupported request keys rather than silently dropping intent. */
        bool Closed(lua_State *state, int index, std::span<const std::string_view> names) {
            if (!lua_istable(state, index))
                return false;
            index = lua_absindex(state, index);
            std::size_t count{};
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                std::size_t bytes{};
                if (const char *key = lua_type(state, -2) == LUA_TSTRING ? lua_tolstring(state, -2, &bytes) : nullptr;
                    ++count > names.size() || !key || std::ranges::find(names, std::string_view{key, bytes}) == names.end()) {
                    lua_pop(state, 2);
                    return false;
                }
                lua_pop(state, 1);
            }
            return true;
        }

        /** @brief Preserves the entire unsigned domain; floating-point identities are never coerced. */
        std::optional<std::uint64_t> Unsigned(lua_State *state, int index) {
            if (lua_isinteger(state, index)) {
                const auto value = lua_tointeger(state, index);
                return value >= 0 ? std::optional{static_cast<std::uint64_t>(value)} : std::nullopt;
            }
            if (lua_type(state, index) != LUA_TSTRING)
                return std::nullopt;
            std::size_t size{};
            const char *text = lua_tolstring(state, index, &size);
            if (size == 0 || size > 20)
                return std::nullopt;
            std::uint64_t value{};
            const auto parsed = std::from_chars(text, text + size, value);
            return parsed.ec == std::errc{} && parsed.ptr == text + size ? std::optional{value} : std::nullopt;
        }

        /** @brief Reads one optional field while restoring the VM stack exactly. */
        std::optional<std::uint64_t> UnsignedField(lua_State *state, int index, const char *name) {
            Field(state, index, name);
            auto value = Unsigned(state, -1);
            lua_pop(state, 1);
            return value;
        }

        /** @brief Bounds raw array keys without invoking authored length or lookup behavior. */
        bool ClosedArray(lua_State *state, int index, std::size_t size) {
            if (!lua_istable(state, index) || lua_rawlen(state, index) != size)
                return false;
            index = lua_absindex(state, index);
            std::size_t count{};
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                if (++count > size || !lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
                    static_cast<std::uint64_t>(lua_tointeger(state, -2)) > size) {
                    lua_pop(state, 2);
                    return false;
                }
                lua_pop(state, 1);
            }
            return true;
        }

        /** @brief Copies a fixed numeric array; native validation remains authoritative for physical values. */
        template <std::size_t N> std::optional<std::array<float, N>> Numbers(lua_State *state, int index) {
            if (!ClosedArray(state, index, N))
                return std::nullopt;
            index = lua_absindex(state, index);
            std::array<float, N> output{};
            for (std::size_t i = 0; i < N; ++i) {
                lua_rawgeti(state, index, static_cast<lua_Integer>(i + 1));
                const auto number = lua_tonumber(state, -1);
                const bool valid =
                    lua_type(state, -1) == LUA_TNUMBER && std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max();
                lua_pop(state, 1);
                if (!valid)
                    return std::nullopt;
                output[i] = static_cast<float>(number);
            }
            return output;
        }

        /** @brief Copies exact slot generations without floating-point conversion or truncation. */
        std::optional<std::array<std::uint32_t, 2>> BodySlots(lua_State *state, int index) {
            if (!ClosedArray(state, index, 2))
                return std::nullopt;
            index = lua_absindex(state, index);
            std::array<std::uint32_t, 2> slots{};
            for (std::size_t i = 0; i < slots.size(); ++i) {
                lua_rawgeti(state, index, static_cast<lua_Integer>(i + 1));
                const auto value = Unsigned(state, -1);
                lua_pop(state, 1);
                if (!value || *value > std::numeric_limits<std::uint32_t>::max() ||
                    (i == 0 && *value == std::numeric_limits<std::uint32_t>::max()) || (i == 1 && *value == 0))
                    return std::nullopt;
                slots[i] = static_cast<std::uint32_t>(*value);
            }
            return slots;
        }

        /** @brief Parses typed collision IDs without admitting another Physics world or native body reference. */
        bool Selectors(lua_State *state, int index, Physics::PhysicsWorldId world, Character::CharacterCollisionSelectors &output) {
            if (constexpr std::array<std::string_view, 3> keys{"layer", "profile", "excluded_body"}; !Closed(state, index, keys))
                return false;
            index = lua_absindex(state, index);
            Field(state, index, "layer");
            if (!lua_isnil(state, -1)) {
                if (lua_type(state, -1) != LUA_TSTRING) {
                    lua_pop(state, 1);
                    return false;
                }
                std::size_t bytes{};
                const char *text = lua_tolstring(state, -1, &bytes);
                auto parsed = Physics::CollisionLayerId::Parse(std::string_view{text, bytes});
                lua_pop(state, 1);
                if (parsed.HasError())
                    return false;
                output.requiredLayer = parsed.Value();
            } else
                lua_pop(state, 1);
            Field(state, index, "profile");
            if (!lua_isnil(state, -1)) {
                if (lua_type(state, -1) != LUA_TSTRING) {
                    lua_pop(state, 1);
                    return false;
                }
                std::size_t bytes{};
                const char *text = lua_tolstring(state, -1, &bytes);
                auto parsed = Physics::CollisionProfileId::Parse(std::string_view{text, bytes});
                lua_pop(state, 1);
                if (parsed.HasError())
                    return false;
                output.requiredProfile = parsed.Value();
            } else
                lua_pop(state, 1);
            Field(state, index, "excluded_body");
            if (!lua_isnil(state, -1)) {
                const auto body = BodySlots(state, -1);
                lua_pop(state, 1);
                if (!body)
                    return false;
                output.excludedBody = Physics::BodyHandle{world, {(*body)[0], (*body)[1]}};
            } else
                lua_pop(state, 1);
            return true;
        }

        /** @brief Maps only admitted optional motion fields into canonical Character request values. */
        bool Motion(lua_State *state, int index, Character::CharacterMovementRequest &output) {
            Field(state, index, "velocity");
            if (!lua_isnil(state, -1)) {
                const auto value = Numbers<3>(state, -1);
                lua_pop(state, 1);
                if (!value)
                    return false;
                output.desiredVelocityMetersPerSecond = Math::Vec3{(*value)[0], (*value)[1], (*value)[2]};
            } else
                lua_pop(state, 1);
            Field(state, index, "heading");
            if (!lua_isnil(state, -1)) {
                const auto value = Numbers<4>(state, -1);
                lua_pop(state, 1);
                if (!value)
                    return false;
                output.desiredHeading = Math::Quaternion{(*value)[0], (*value)[1], (*value)[2], (*value)[3]};
            } else
                lua_pop(state, 1);
            Field(state, index, "jump");
            if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
                lua_pop(state, 1);
                return false;
            }
            output.jumpRequested = lua_toboolean(state, -1);
            lua_pop(state, 1);
            Field(state, index, "stance");
            if (!lua_isnil(state, -1)) {
                const auto value = Unsigned(state, -1);
                lua_pop(state, 1);
                if (!value || *value > static_cast<std::uint64_t>(Character::CharacterStanceIntent::Crouch))
                    return false;
                output.stance = static_cast<Character::CharacterStanceIntent>(*value);
            } else
                lua_pop(state, 1);
            Field(state, index, "shape");
            if (!lua_isnil(state, -1)) {
                const auto value = Numbers<2>(state, -1);
                lua_pop(state, 1);
                if (!value)
                    return false;
                output.shapeChange = Character::CharacterShapeChangeRequest{{(*value)[0], (*value)[1]}};
            } else
                lua_pop(state, 1);
            return true;
        }
    }  // namespace

    /** @copydoc ReadLuaCharacterMovement */
    Result<Character::CharacterMovementRequest> ReadLuaCharacterMovement(lua_State *state, int index,
                                                                         const Character::CharacterControllerHandle handle,
                                                                         const Physics::PhysicsWorldId physicsWorld) {
        using Output = Result<Character::CharacterMovementRequest>;
        if (constexpr std::array<std::string_view, 8> keys{"tick", "correlation", "velocity", "heading", "jump", "stance", "shape",
                                                           "filter"};
            !Closed(state, index, keys))
            return Output::Failure(MakeError(Character::CharacterErrors::RequestInvalid));
        index = lua_absindex(state, index);
        const auto tick = UnsignedField(state, index, "tick");
        const auto sequence = UnsignedField(state, index, "correlation");
        if (!tick.has_value() || !sequence.has_value())
            return Output::Failure(MakeError(Character::CharacterErrors::RequestInvalid));
        Character::CharacterMovementRequest output{.controller = handle, .tick = *tick, .sequence = *sequence};
        if (!Motion(state, index, output))
            return Output::Failure(MakeError(Character::CharacterErrors::RequestInvalid));
        Field(state, index, "filter");
        if (!lua_isnil(state, -1)) {
            Character::CharacterCollisionSelectors selectors;
            const bool valid = Selectors(state, -1, physicsWorld, selectors);
            lua_pop(state, 1);
            if (!valid)
                return Output::Failure(MakeError(Character::CharacterErrors::RequestInvalid));
            output.filterChange = selectors;
        } else
            lua_pop(state, 1);
        return Output::Success(std::move(output));
    }

    /** @copydoc PushCharacterUnsigned */
    void PushCharacterUnsigned(lua_State *state, const std::uint64_t value) {
        std::array<char, 20> text{};
        const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
        lua_pushlstring(state, text.data(), static_cast<std::size_t>(result.ptr - text.data()));
    }
}  // namespace Horo::Gameplay::Detail
