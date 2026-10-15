#include "LuaNavigationBinding.h"

#include "Horo/Gameplay/GameplayNavigationContext.h"

#include <array>
#include <limits>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        constexpr const char *ScopeType = "horo.navigation.scope";
        constexpr const char *HandleType = "horo.navigation.request";

        struct Scope final {
            std::shared_ptr<const GameplayNavigationContext> context;
            CancellationToken lifetime;
        };

        /** @brief Finalize owned values without dereferencing Scene or provider state. */
        template <class T> int Collect(lua_State *state) {
            std::destroy_at(static_cast<T *>(lua_touserdata(state, 1)));
            return 0;
        }

        /** @brief Preserve canonical typed error identities in the script ABI. */
        int Failure(lua_State *state, const Error &error) {
            lua_pushnil(state);
            lua_newtable(state);
            lua_pushstring(state, error.code.Value().c_str());
            lua_setfield(state, -2, "code");
            lua_pushstring(state, error.domain.Value().c_str());
            lua_setfield(state, -2, "domain");
            return 2;
        }

        /** @brief Project one immutable finite vector. */
        void Vector(lua_State *state, const Math::Vec3 value) {
            lua_createtable(state, 3, 0);
            const std::array coordinates{value.x, value.y, value.z};
            for (std::size_t i = 0; i < coordinates.size(); ++i) {
                lua_pushnumber(state, coordinates[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        /** @brief Validate scalar shape before constructing any owned C++ request. */
        Math::Vec3 ReadVector(lua_State *state, int first) {
            return {static_cast<float>(luaL_checknumber(state, first)), static_cast<float>(luaL_checknumber(state, first + 1)),
                    static_cast<float>(luaL_checknumber(state, first + 2))};
        }

        /** @brief Set a copied numeric field without exposing native pointer identity. */
        template <class T> void Number(lua_State *state, const char *name, T value) {
            lua_pushnumber(state, static_cast<lua_Number>(value));
            lua_setfield(state, -2, name);
        }

        /** @brief Preserve exact 64-bit identity bytes as an opaque decimal string. */
        void Identity(lua_State *state, const char *name, std::uint64_t value) {
            const auto text = std::to_string(value);
            lua_pushlstring(state, text.data(), text.size());
            lua_setfield(state, -2, name);
        }

        /** @brief Copy the same Horo polygon provenance retained by a native result. */
        void Provenance(lua_State *state, const Navigation::NavigationQueryProvenance &value) {
            lua_newtable(state);
            Identity(state, "world", value.world.Value());
            Identity(state, "topology", value.topology.Value());
            Identity(state, "surface", value.surface.Value());
            Number(state, "polygon_index", value.polygonIndex);
        }

        /** @brief Copy bounded owned corridor evidence into the script result. */
        void Corridor(lua_State *state, const Navigation::NavigationPath &path) {
            lua_createtable(state, static_cast<int>(path.corridor.size()), 0);
            for (std::size_t i = 0; i < path.corridor.size(); ++i) {
                lua_newtable(state);
                Provenance(state, path.corridor[i].provenance);
                lua_setfield(state, -2, "provenance");
                Identity(state, "area", path.corridor[i].area.Value());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        /** @brief Copy bounded owned portals evidence into the script result. */
        void Portals(lua_State *state, const Navigation::NavigationPath &path) {
            lua_createtable(state, static_cast<int>(path.portals.size()), 0);
            for (std::size_t i = 0; i < path.portals.size(); ++i) {
                const auto &portal = path.portals[i];
                lua_newtable(state);
                Number(state, "kind", portal.kind);
                Number(state, "from_polygon_index", portal.fromPolygonIndex);
                Number(state, "to_polygon_index", portal.toPolygonIndex);
                Number(state, "left_vertex_index", portal.leftVertexIndex);
                Number(state, "right_vertex_index", portal.rightVertexIndex);
                Number(state, "width_meters", portal.widthMeters);
                Number(state, "clearance_meters", portal.clearanceMeters);
                Vector(state, portal.left);
                lua_setfield(state, -2, "left");
                Vector(state, portal.right);
                lua_setfield(state, -2, "right");
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        /** @brief Copy bounded owned waypoints evidence into the script result. */
        void Waypoints(lua_State *state, const Navigation::NavigationPath &path) {
            lua_createtable(state, static_cast<int>(path.waypoints.size()), 0);
            for (std::size_t i = 0; i < path.waypoints.size(); ++i) {
                const auto &point = path.waypoints[i];
                lua_newtable(state);
                Vector(state, point.position);
                lua_setfield(state, -2, "position");
                lua_newtable(state);
                Number(state, "kind", point.provenance.kind);
                Number(state, "corridor_polygon_index", point.provenance.corridorPolygonIndex);
                Number(state, "portal_index", point.provenance.portalIndex);
                Number(state, "vertex_index", point.provenance.vertexIndex);
                Provenance(state, point.provenance.polygon);
                lua_setfield(state, -2, "polygon");
                lua_setfield(state, -2, "provenance");
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        /** @brief Retain path completion meaning and every owned corridor/portal/waypoint proof. */
        void Path(lua_State *state, const Navigation::NavigationPath &path) {
            lua_newtable(state);
            Number(state, "status", path.status);
            Number(state, "stop_reason", path.stopReason);
            Number(state, "stop_polygon_index", path.stopPolygonIndex);
            Number(state, "cost", path.cost);
            Number(state, "length_meters", path.lengthMeters);
            Identity(state, "source_generation", path.sourceGeneration.Value());
            Vector(state, path.stopPosition);
            lua_setfield(state, -2, "stop_position");
            Corridor(state, path);
            lua_setfield(state, -2, "corridor");
            Portals(state, path);
            lua_setfield(state, -2, "portals");
            Waypoints(state, path);
            lua_setfield(state, -2, "waypoints");
            lua_createtable(state, static_cast<int>(path.points.size()), 0);
            for (std::size_t i = 0; i < path.points.size(); ++i) {
                Vector(state, path.points[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "points");
        }

        /** @brief Nonblocking owner-thread observation; terminal failures keep the native error. */
        int Poll(lua_State *state) {
            const auto &handle = *static_cast<GameplayNavigationHandle *>(luaL_checkudata(state, 1, HandleType));
            auto result = handle.Poll();
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            if (!result.Value()) {
                lua_pushnil(state);
                return 1;
            }
            lua_newtable(state);
            const auto &value = *result.Value();
            if (value.path) {
                Path(state, *value.path);
                lua_setfield(state, -2, "path");
            }
            if (value.movement) {
                lua_newtable(state);
                lua_pushinteger(state, static_cast<lua_Integer>(value.movement->state));
                lua_setfield(state, -2, "state");
                lua_newtable(state);
                Identity(state, "sequence", value.movement->id.sequence);
                Identity(state, "world", value.movement->id.agent.world.Value());
                Number(state, "agent_index", value.movement->id.agent.slot.index);
                Number(state, "agent_generation", value.movement->id.agent.slot.generation);
                lua_setfield(state, -2, "id");
                if (value.movement->error) {
                    Failure(state, *value.movement->error);
                    lua_setfield(state, -3, "error");
                    lua_pop(state, 1);
                }
                lua_setfield(state, -2, "movement");
            }
            return 1;
        }

        /** @brief Delegate cancellation to the shared native operation fence. */
        int Cancel(lua_State *state) {
            const auto &handle = *static_cast<GameplayNavigationHandle *>(luaL_checkudata(state, 1, HandleType));
            auto result = handle.Cancel();
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            lua_pushboolean(state, result.Value());
            return 1;
        }

        /** @brief Marshal primitive inputs into the same bounded host policy used by native callers. */
        template <GameplayNavigationOperation Operation> int Submit(lua_State *state) {
            const auto &scope = *static_cast<Scope *>(luaL_checkudata(state, lua_upvalueindex(1), ScopeType));
            Math::Vec3 start{};
            Math::Vec3 destination{};
            if constexpr (Operation == GameplayNavigationOperation::Path) {
                start = ReadVector(state, 1);
                destination = ReadVector(state, 4);
            } else
                destination = ReadVector(state, 1);
            lua_Integer index{};
            lua_Integer generation{};
            if constexpr (Operation == GameplayNavigationOperation::Follow) {
                index = luaL_checkinteger(state, 4);
                generation = luaL_checkinteger(state, 5);
                if (index < 0 || index > std::numeric_limits<std::uint32_t>::max() || generation <= 0 ||
                    generation > std::numeric_limits<std::uint32_t>::max())
                    return Failure(state, MakeError(Navigation::NavigationErrors::IdentityInvalid));
            }
            if (!scope.context)
                return Failure(state, MakeError(Navigation::NavigationErrors::CapabilityUnavailable));
            GameplayNavigationRequest request{.operation = Operation,
                                              .start = start,
                                              .destination = destination,
                                              .cancellation = scope.lifetime,
                                              .context = Telemetry::CaptureOperationContext()};
            if constexpr (Operation == GameplayNavigationOperation::Follow)
                request.target = Navigation::NavigationAgentOwner{scope.context->Binding().scene.scene, static_cast<std::uint32_t>(index),
                                                                  static_cast<std::uint32_t>(generation)};
            auto result = scope.context->Submit(std::move(request));
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            auto *storage = static_cast<GameplayNavigationHandle *>(lua_newuserdatauv(state, sizeof(GameplayNavigationHandle), 0));
            std::construct_at(storage, std::move(result).Value());
            if (luaL_newmetatable(state, HandleType)) {
                lua_pushboolean(state, false);
                lua_setfield(state, -2, "__metatable");
                lua_pushcfunction(state, Collect<GameplayNavigationHandle>);
                lua_setfield(state, -2, "__gc");
                lua_newtable(state);
                lua_pushcfunction(state, Poll);
                lua_setfield(state, -2, "poll");
                lua_pushcfunction(state, Cancel);
                lua_setfield(state, -2, "cancel");
                lua_setfield(state, -2, "__index");
            }
            lua_setmetatable(state, -2);
            return 1;
        }
    }  // namespace

    /** @copydoc PushLuaNavigationContext */
    void PushLuaNavigationContext(lua_State *state, const BehaviorContext &context, CancellationToken lifetime) {
        lua_newtable(state);
        auto *scope = static_cast<Scope *>(lua_newuserdatauv(state, sizeof(Scope), 0));
        std::construct_at(scope, Scope{context.NavigationContext(), std::move(lifetime)});
        if (luaL_newmetatable(state, ScopeType)) {
            lua_pushboolean(state, false);
            lua_setfield(state, -2, "__metatable");
            lua_pushcfunction(state, Collect<Scope>);
            lua_setfield(state, -2, "__gc");
        }
        lua_setmetatable(state, -2);
        const auto bind = [state]<lua_CFunction Function>(const char *name) {
            lua_pushvalue(state, -1);
            lua_pushcclosure(state, Function, 1);
            lua_setfield(state, -3, name);
        };
        bind.operator()<Submit<GameplayNavigationOperation::Path>>("path");
        bind.operator()<Submit<GameplayNavigationOperation::MoveTo>>("move_to");
        bind.operator()<Submit<GameplayNavigationOperation::Follow>>("follow");
        lua_pop(state, 1);
    }
}  // namespace Horo::Gameplay::Detail
