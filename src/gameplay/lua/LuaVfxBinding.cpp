#include "LuaVfxBinding.h"

#include "Horo/Gameplay/GameplayVfxContext.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <new>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        constexpr auto Metatable = "horo.gameplay.vfx.scope";

        struct Holder final {
            std::shared_ptr<const GameplayVfxContext> context;
        };

        [[nodiscard]] Holder &Scope(lua_State *state) noexcept {
            return *static_cast<Holder *>(lua_touserdata(state, lua_upvalueindex(1)));
        }

        int Destroy(lua_State *state) {
            static_cast<Holder *>(lua_touserdata(state, 1))->~Holder();
            return 0;
        }

        void Field(lua_State *state, int table, const char *name) {
            lua_pushstring(state, name);
            lua_rawget(state, table);
        }

        [[nodiscard]] bool Integer(lua_State *state, int index, std::int64_t &value) noexcept {
            if (!lua_isinteger(state, index))
                return false;
            value = static_cast<std::int64_t>(lua_tointeger(state, index));
            return true;
        }

        [[nodiscard]] bool Number(lua_State *state, int index, float &value) noexcept {
            if (lua_type(state, index) != LUA_TNUMBER)
                return false;
            const auto number = lua_tonumber(state, index);
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                return false;
            value = static_cast<float>(number);
            return true;
        }

        [[nodiscard]] bool Position(lua_State *state, int table, GameplayVfxEventPayload &payload) {
            std::array<std::int64_t, 3> coordinates{};
            constexpr std::array names{"x_mm", "y_mm", "z_mm"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                Field(state, table, names[index]);
                const bool valid = Integer(state, -1, coordinates[index]);
                lua_pop(state, 1);
                if (!valid)
                    return false;
            }
            payload.transform.position = Math::WorldCoordinate64::FromMillimeters(coordinates[0], coordinates[1], coordinates[2]);
            return true;
        }

        template <std::size_t Count>
        [[nodiscard]] bool OptionalVector(lua_State *state, int table, const char *name, std::array<float, Count> &values, bool &present) {
            Field(state, table, name);
            present = !lua_isnil(state, -1);
            if (!present) {
                lua_pop(state, 1);
                return true;
            }
            if (!lua_istable(state, -1) || lua_rawlen(state, -1) != Count) {
                lua_pop(state, 1);
                return false;
            }
            bool valid = true;
            for (std::size_t index = 0; index < Count; ++index) {
                lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
                valid = Number(state, -1, values[index]) && valid;
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
            return valid;
        }

        [[nodiscard]] bool Placement(lua_State *state, int table, GameplayVfxEventPayload &payload) {
            if (!Position(state, table, payload))
                return false;
            Field(state, table, "scale");
            const bool scaleValid = lua_isnil(state, -1) || Number(state, -1, payload.scale);
            lua_pop(state, 1);
            std::array<float, 4> orientation{};
            std::array<float, 3> normal{}, axes{};
            bool rotated{}, impacted{}, scaled{};
            if (!scaleValid || !OptionalVector(state, table, "orientation", orientation, rotated) ||
                !OptionalVector(state, table, "normal", normal, impacted) || !OptionalVector(state, table, "axes", axes, scaled))
                return false;
            if (rotated)
                payload.transform.orientation = {orientation[0], orientation[1], orientation[2], orientation[3]};
            if (impacted)
                payload.impactNormal = Math::Vec3{normal[0], normal[1], normal[2]};
            if (scaled)
                payload.transform.scale = {axes[0], axes[1], axes[2]};
            return true;
        }

        [[nodiscard]] bool Parameter(lua_State *state, Vfx::VfxParameterValue &value) {
            if (!lua_istable(state, -1) || lua_rawlen(state, -1) != 2)
                return false;
            lua_rawgeti(state, -1, 1);
            std::int64_t channel{};
            const bool valid = Integer(state, -1, channel) && channel >= 0 && channel <= UINT16_MAX;
            lua_pop(state, 1);
            lua_rawgeti(state, -1, 2);
            const bool number = Number(state, -1, value.value);
            lua_pop(state, 1);
            value.channel = static_cast<std::uint16_t>(channel);
            return valid && number;
        }

        [[nodiscard]] bool Parameters(lua_State *state, int table, GameplayVfxEventPayload &payload) {
            Field(state, table, "parameters");
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                return true;
            }
            if (!lua_istable(state, -1) || lua_rawlen(state, -1) > Vfx::VfxSpawnHardLimits::Parameters) {
                lua_pop(state, 1);
                return false;
            }
            payload.parameters.count = static_cast<std::uint32_t>(lua_rawlen(state, -1));
            bool valid = true;
            for (std::uint32_t index = 0; index < payload.parameters.count; ++index) {
                lua_rawgeti(state, -1, index + 1);
                valid = Parameter(state, payload.parameters.values[index]) && valid;
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
            return valid;
        }

        [[nodiscard]] bool Schema(lua_State *state, int table, GameplayVfxEventPayload &payload) {
            Field(state, table, "schema");
            std::int64_t schema{};
            const bool valid = lua_isnil(state, -1) || (Integer(state, -1, schema) && schema > 0);
            lua_pop(state, 1);
            payload.parameters.schema = static_cast<std::uint64_t>(schema);
            return valid;
        }

        int Result(lua_State *state, const GameplayVfxSubmission submission) {
            lua_pushinteger(state, static_cast<lua_Integer>(submission.status));
            std::array<char, 20> sequence{};
            const auto encoded = std::to_chars(sequence.data(), sequence.data() + sequence.size(), submission.receipt.occurrence.sequence);
            lua_pushlstring(state, sequence.data(), static_cast<std::size_t>(encoded.ptr - sequence.data()));
            return 2;
        }

        int Emit(lua_State *state) {
            const auto &context = Scope(state).context;
            if (!context)
                return Result(state, {});
            GameplayVfxLookup lookup;
            if (lua_type(state, 1) == LUA_TSTRING) {
                std::size_t size{};
                const auto *event = lua_tolstring(state, 1, &size);
                lookup = context->ResolveEvent({event, size});
            } else if (lua_isinteger(state, 1) && lua_tointeger(state, 1) > 0) {
                lookup = context->ResolveTag({static_cast<std::uint64_t>(lua_tointeger(state, 1))});
            } else {
                return Result(state, {.status = Vfx::VfxSpawnStatus::InvalidRequest});
            }
            if (lookup.status != Vfx::VfxSpawnStatus::Accepted)
                return Result(state, {.status = lookup.status});
            GameplayVfxEventPayload payload;
            if (!lua_istable(state, 2) || !Placement(state, 2, payload) || !Schema(state, 2, payload) || !Parameters(state, 2, payload))
                return Result(state, {.status = Vfx::VfxSpawnStatus::InvalidRequest});
            return Result(state, context->Emit(lookup.key, payload));
        }
    }  // namespace

    /** @copydoc PushLuaVfxContext */
    void PushLuaVfxContext(lua_State *state, const std::shared_ptr<const GameplayVfxContext> &context) {
        if (!context) {
            lua_pushnil(state);
            return;
        }
        lua_newtable(state);
        // No C++ owner is constructed until every fallible metatable allocation has completed.
        if (luaL_newmetatable(state, Metatable)) {
            lua_pushboolean(state, false);
            lua_setfield(state, -2, "__metatable");
            lua_pushcfunction(state, Destroy);
            lua_setfield(state, -2, "__gc");
        }
        auto *holder = static_cast<Holder *>(lua_newuserdatauv(state, sizeof(Holder), 0));
        new (holder) Holder{context};
        lua_pushvalue(state, -2);
        lua_setmetatable(state, -2);
        lua_remove(state, -2);
        lua_pushcclosure(state, Emit, 1);
        lua_setfield(state, -2, "emit");
    }
}  // namespace Horo::Gameplay::Detail
