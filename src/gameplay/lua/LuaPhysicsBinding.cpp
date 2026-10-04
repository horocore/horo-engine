#include "LuaPhysicsBinding.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include <array>
#include <memory>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        constexpr const char *ContextType = "horo.gameplay.physics.context";
        constexpr const char *ClientType = "horo.physics.query_event_capability";
        constexpr const char *BatchType = "horo.physics.query_batch";
        using ContextHolder = std::shared_ptr<const GameplayPhysicsContext>;
        using Client = Physics::PhysicsQueryEventCapability;
        using Batch = Physics::PhysicsQueryBatchHandle;

        /** @brief Preserve the canonical domain and error code without throwing a Lua exception. */
        int Failure(lua_State *state, const Error &error) {
            lua_pushnil(state);
            lua_newtable(state);
            lua_pushstring(state, error.code.Value().c_str());
            lua_setfield(state, -2, "code");
            lua_pushstring(state, error.domain.Value().c_str());
            lua_setfield(state, -2, "domain");
            return 2;
        }

        /** @brief Destroy a VM-owned value; no finalizer controls world or module retirement. */
        template <class T> int Collect(lua_State *state) {
            std::destroy_at(static_cast<T *>(lua_touserdata(state, 1)));
            return 0;
        }

        /** @brief Copy an inert vector into a bounded Lua value. */
        void Vector(lua_State *state, const Math::Vec3 value) {
            lua_createtable(state, 3, 0);
            const std::array coordinates{value.x, value.y, value.z};
            for (std::size_t index = 0; index < coordinates.size(); ++index) {
                lua_pushnumber(state, coordinates[index]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
            }
        }

        /** @brief Read bounded primitive coordinates; validation of finite/unit values remains Physics-owned. */
        Math::Vec3 ReadVector(lua_State *state, const int first) {
            return {static_cast<float>(luaL_checknumber(state, first)), static_cast<float>(luaL_checknumber(state, first + 1)),
                    static_cast<float>(luaL_checknumber(state, first + 2))};
        }

        /** @brief Expose copied access identity, never a pointer or native solver handle. */
        int Identity(lua_State *state) {
            const auto &client = *static_cast<Client *>(luaL_checkudata(state, 1, ClientType));
            const auto identity = client.Identity();
            lua_pushinteger(state, static_cast<lua_Integer>(identity.world.Value()));
            lua_pushinteger(state, static_cast<lua_Integer>(identity.capabilityGeneration));
            return 2;
        }

        /** @brief Marshal a bounded ray into the existing typed command; Physics remains validation authority. */
        Result<Physics::PhysicsQueryCommand> Command(lua_State *state) {
            const auto &client = *static_cast<Client *>(luaL_checkudata(state, 1, ClientType));
            const auto revision = luaL_checkinteger(state, 2);
            const auto generation = luaL_checkinteger(state, 3);
            const std::string_view channelText = luaL_checkstring(state, 4);
            const auto origin = ReadVector(state, 5);
            const auto direction = ReadVector(state, 8);
            const auto distance = static_cast<float>(luaL_checknumber(state, 11));
            const auto maximumHits = luaL_optinteger(state, 12, 1);
            if (revision <= 0 || generation <= 0 || maximumHits <= 0 || maximumHits > 64)
                return Result<Physics::PhysicsQueryCommand>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            auto channel = Physics::PhysicsQueryChannelId::Parse(channelText);
            if (channel.HasError())
                return Result<Physics::PhysicsQueryCommand>::Failure(channel.ErrorValue());
            Physics::PhysicsQueryDescriptor descriptor;
            descriptor.world = client.Identity().world;
            descriptor.sceneGeneration = static_cast<std::uint64_t>(generation);
            descriptor.geometry = Physics::PhysicsRayQuery{origin, direction, distance};
            descriptor.filter.channel = std::move(channel).Value();
            descriptor.maximumHitCount = static_cast<std::uint32_t>(maximumHits);
            return Result<Physics::PhysicsQueryCommand>::Success({client.Identity(), static_cast<std::uint64_t>(revision), descriptor});
        }

        /** @brief Project bounded copied hit values without retaining native storage. */
        void Hits(lua_State *state, const Physics::PhysicsQueryCompletion &completion,
                  const std::span<const Physics::PhysicsQueryHit> hits) {
            lua_createtable(state, static_cast<int>(completion.result.hitCount), 3);
            for (std::uint32_t index = 0; index < completion.result.hitCount; ++index) {
                lua_newtable(state);
                Vector(state, hits[index].position);
                lua_setfield(state, -2, "position");
                if (hits[index].normal) {
                    Vector(state, *hits[index].normal);
                    lua_setfield(state, -2, "normal");
                }
                lua_pushnumber(state, hits[index].distanceMeters);
                lua_setfield(state, -2, "distance_meters");
                lua_rawseti(state, -2, index + 1);
            }
            lua_pushboolean(state, completion.result.truncated);
            lua_setfield(state, -2, "truncated");
            lua_pushinteger(state, static_cast<lua_Integer>(completion.completedTick));
            lua_setfield(state, -2, "completed_tick");
            lua_pushinteger(state, static_cast<lua_Integer>(completion.publicationRevision));
            lua_setfield(state, -2, "publication_revision");
        }

        /** @brief Invoke the Physics-owned immediate operation, preserving typed failures. */
        int Submit(lua_State *state) {
            const auto &client = *static_cast<Client *>(luaL_checkudata(state, 1, ClientType));
            const auto command = Command(state);
            if (command.HasError())
                return Failure(state, command.ErrorValue());
            std::array<Physics::PhysicsQueryHit, 64> hits{};
            const auto submitted = client.Submit(command.Value(), hits);
            if (submitted.HasError())
                return Failure(state, submitted.ErrorValue());
            Hits(state, submitted.Value(), hits);
            return 1;
        }

        /** @brief Observe the existing batch's immutable terminal result without waiting. */
        int Poll(lua_State *state) {
            const auto &batch = *static_cast<Batch *>(luaL_checkudata(state, 1, BatchType));
            const auto polled = batch.Poll();
            if (polled.HasError())
                return Failure(state, polled.ErrorValue());
            if (!polled.Value()) {
                lua_pushnil(state);
                return 1;
            }
            lua_createtable(state, static_cast<int>(polled.Value()->entries.size()), 0);
            for (std::size_t index = 0; index < polled.Value()->entries.size(); ++index) {
                const auto &entry = polled.Value()->entries[index];
                Hits(state, entry.completion, entry.hits);
                lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
            }
            return 1;
        }

        /** @brief Delegate cancellation to the Physics terminal publication gate. */
        int Cancel(lua_State *state) {
            const auto &batch = *static_cast<Batch *>(luaL_checkudata(state, 1, BatchType));
            lua_pushboolean(state, batch.Cancel());
            return 1;
        }

        /** @brief Queue one bounded owned command through the existing SubmitBatch interface. */
        int SubmitBatch(lua_State *state) {
            const auto &client = *static_cast<Client *>(luaL_checkudata(state, 1, ClientType));
            const auto command = Command(state);
            if (command.HasError())
                return Failure(state, command.ErrorValue());
            auto submitted = client.SubmitBatch(std::span{&command.Value(), 1});
            if (submitted.HasError())
                return Failure(state, submitted.ErrorValue());
            auto *storage = static_cast<Batch *>(lua_newuserdatauv(state, sizeof(Batch), 0));
            std::construct_at(storage, std::move(submitted).Value());
            if (luaL_newmetatable(state, BatchType)) {
                lua_pushboolean(state, false);
                lua_setfield(state, -2, "__metatable");
                lua_pushcfunction(state, Collect<Batch>);
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

        /** @brief Copy completed-tick event evidence through the existing ReadEvents operation. */
        int ReadEvents(lua_State *state) {
            const auto &client = *static_cast<Client *>(luaL_checkudata(state, 1, ClientType));
            const auto tick = luaL_checkinteger(state, 2);
            const auto revision = luaL_checkinteger(state, 3);
            const auto maximum = luaL_optinteger(state, 4, 64);
            if (tick <= 0 || revision <= 0 || maximum <= 0 || maximum > 64)
                return Failure(state, MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            std::array<Physics::PhysicsEventRecord, 64> records{};
            const auto read = client.ReadEvents({client.Identity(), static_cast<std::uint64_t>(tick), static_cast<std::uint64_t>(revision),
                                                 static_cast<std::uint32_t>(maximum)},
                                                records);
            if (read.HasError())
                return Failure(state, read.ErrorValue());
            lua_createtable(state, static_cast<int>(read.Value().recordCount), 3);
            for (std::uint32_t index = 0; index < read.Value().recordCount; ++index) {
                lua_newtable(state);
                lua_pushinteger(state, static_cast<lua_Integer>(records[index].simulationTick));
                lua_setfield(state, -2, "simulation_tick");
                lua_pushinteger(state, static_cast<lua_Integer>(records[index].kind));
                lua_setfield(state, -2, "kind");
                lua_rawseti(state, -2, index + 1);
            }
            lua_pushboolean(state, read.Value().truncated);
            lua_setfield(state, -2, "truncated");
            lua_pushinteger(state, static_cast<lua_Integer>(read.Value().omittedRecordCount));
            lua_setfield(state, -2, "omitted_record_count");
            lua_pushinteger(state, static_cast<lua_Integer>(read.Value().droppedRecordCount));
            lua_setfield(state, -2, "dropped_record_count");
            return 1;
        }

        /** @brief Acquire only the principal and exact epoch injected by the trusted host. */
        int Acquire(lua_State *state) {
            const auto &holder = *static_cast<ContextHolder *>(lua_touserdata(state, lua_upvalueindex(1)));
            if (!holder)
                return Failure(state, MakeError(GameplayErrors::PhysicsUnavailable));
            const auto &binding = holder->Binding();
            auto acquired = holder->Acquire(binding.moduleId, binding.scene, binding.sceneGeneration);
            if (acquired.HasError())
                return Failure(state, acquired.ErrorValue());
            auto *storage = static_cast<Client *>(lua_newuserdatauv(state, sizeof(Client), 0));
            std::construct_at(storage, std::move(acquired).Value());
            if (luaL_newmetatable(state, ClientType)) {
                lua_pushboolean(state, false);
                lua_setfield(state, -2, "__metatable");
                lua_pushcfunction(state, Collect<Client>);
                lua_setfield(state, -2, "__gc");
                lua_newtable(state);
                lua_pushcfunction(state, Identity);
                lua_setfield(state, -2, "identity");
                lua_pushcfunction(state, Submit);
                lua_setfield(state, -2, "submit");
                lua_pushcfunction(state, SubmitBatch);
                lua_setfield(state, -2, "submit_batch");
                lua_pushcfunction(state, ReadEvents);
                lua_setfield(state, -2, "read_events");
                lua_setfield(state, -2, "__index");
            }
            lua_setmetatable(state, -2);
            return 1;
        }
    }  // namespace

    /** @copydoc PushLuaPhysicsContext */
    void PushLuaPhysicsContext(lua_State *state, const BehaviorContext &context) {
        lua_newtable(state);
        const auto physics = context.PhysicsContext();
        if (physics) {
            lua_pushinteger(state, static_cast<lua_Integer>(physics->Binding().scene));
            lua_setfield(state, -2, "scene");
            lua_pushinteger(state, static_cast<lua_Integer>(physics->Binding().sceneGeneration));
            lua_setfield(state, -2, "scene_generation");
        }
        auto *holder = static_cast<ContextHolder *>(lua_newuserdatauv(state, sizeof(ContextHolder), 0));
        std::construct_at(holder, physics);
        if (luaL_newmetatable(state, ContextType)) {
            lua_pushboolean(state, false);
            lua_setfield(state, -2, "__metatable");
            lua_pushcfunction(state, Collect<ContextHolder>);
            lua_setfield(state, -2, "__gc");
        }
        lua_setmetatable(state, -2);
        lua_pushcclosure(state, Acquire, 1);
        lua_setfield(state, -2, "acquire");
    }
}  // namespace Horo::Gameplay::Detail
