#include "LuaCharacterBinding.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Physics/CharacterErrors.h"

#include <algorithm>
#include <limits>
#include <new>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        constexpr const char *ContextType = "horo.character.context";
        constexpr const char *ClientType = "horo.character.client";
        constexpr const char *HandleType = "horo.character.controller";
        using Client = Character::CharacterCapability;
        using Handle = Character::CharacterControllerHandle;

        struct Context final {
            std::shared_ptr<const GameplayCharacterContext> scope;
            std::optional<Error> error;
        };

        /** @brief Returns the original typed failure without raising a script exception. */
        int Failure(lua_State *state, const Error &error) {
            lua_pushnil(state);
            lua_createtable(state, 0, 3);
            lua_pushstring(state, error.code.Value().c_str());
            lua_setfield(state, -2, "code");
            lua_pushstring(state, error.domain.Value().c_str());
            lua_setfield(state, -2, "domain");
            lua_pushlstring(state, error.message.data(), error.message.size());
            lua_setfield(state, -2, "message");
            return 2;
        }

        /** @brief Keeps native allocation failure inside the typed script boundary, without allocating a C++ error. */
        template <lua_CFunction Callback> int Guarded(lua_State *state) {
            try {
                return Callback(state);
            } catch (const std::bad_alloc &) {
                lua_pushnil(state);
                lua_createtable(state, 0, 3);
                lua_pushstring(state, Character::CharacterErrors::CapacityExceeded.code.Value().c_str());
                lua_setfield(state, -2, "code");
                lua_pushstring(state, Character::CharacterErrors::CapacityExceeded.domain.Value().c_str());
                lua_setfield(state, -2, "domain");
                lua_pushliteral(state, "Character request storage is unavailable.");
                lua_setfield(state, -2, "message");
                return 2;
            }
        }

        /** @brief Destroys only copied Horo storage; world retirement is host-owned. */
        template <class T> int Collect(lua_State *state) {
            std::destroy_at(static_cast<T *>(lua_touserdata(state, 1)));
            return 0;
        }

        /** @brief Owns one userdata value behind a locked private type. */
        template <class T> void Push(lua_State *state, T value, const char *name) {
            auto *owned = static_cast<T *>(lua_newuserdatauv(state, sizeof(T), 0));
            std::construct_at(owned, std::move(value));
            if (luaL_newmetatable(state, name)) {
                lua_pushboolean(state, false);
                lua_setfield(state, -2, "__metatable");
                lua_pushcfunction(state, Collect<T>);
                lua_setfield(state, -2, "__gc");
            }
            lua_setmetatable(state, -2);
        }

        /** @brief Reads the immutable client value bound by acquisition. */
        const Client &ReadClient(lua_State *state) {
            return *static_cast<Client *>(luaL_checkudata(state, 1, ClientType));
        }

        /** @brief Reads only an opaque Horo controller handle. */
        Handle ReadHandle(lua_State *state) {
            return *static_cast<Handle *>(luaL_checkudata(state, 2, HandleType));
        }

        /** @brief Projects exact routing evidence without a native pointer. */
        int Identity(lua_State *state) {
            const auto identity = ReadClient(state).Identity();
            PushCharacterUnsigned(state, identity.sceneGeneration);
            PushCharacterUnsigned(state, identity.world.Value());
            PushCharacterUnsigned(state, identity.physicsWorld.Value());
            PushCharacterUnsigned(state, identity.generation);
            return 4;
        }

        /** @brief Checks live access without borrowing a controller or world. */
        int Validate(lua_State *state) {
            const auto result = ReadClient(state).Validate();
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            lua_pushboolean(state, true);
            return 1;
        }

        /** @brief Resolves a caller-owned slot incarnation only through canonical Character validation. */
        int Controller(lua_State *state) {
            const auto slot = luaL_checkinteger(state, 2), generation = luaL_checkinteger(state, 3);
            if (slot < 0 || slot >= std::numeric_limits<std::uint32_t>::max() || generation <= 0 ||
                generation > std::numeric_limits<std::uint32_t>::max())
                return Failure(state, MakeError(Character::CharacterErrors::HandleMalformed));
            const auto &client = ReadClient(state);
            const auto identity = client.Identity();
            const Handle handle{identity.sceneGeneration,
                                identity.world,
                                {static_cast<std::uint32_t>(slot), static_cast<std::uint32_t>(generation)}};
            const auto checked = client.ControllerDescriptor(handle);
            if (checked.HasError())
                return Failure(state, checked.ErrorValue());
            Push(state, handle, HandleType);
            return 1;
        }

        /** @brief Submits one bounded owned intent without retaining script storage or changing tick advancement. */
        int Queue(lua_State *state) {
            const auto handle = ReadHandle(state);
            const auto &client = ReadClient(state);
            auto command = ReadLuaCharacterMovement(state, 3, handle, client.Identity().physicsWorld);
            if (command.HasError())
                return Failure(state, command.ErrorValue());
            auto result = client.QueueMovementCommand(command.Value());
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            lua_pushinteger(state, static_cast<lua_Integer>(result.Value().status));
            return 1;
        }

        /** @brief Copies immutable committed locomotion facts, retaining exact unsigned tick and producer correlation. */
        int Snapshot(lua_State *state) {
            const auto handle = ReadHandle(state);
            auto result = ReadClient(state).ControllerLocomotionSnapshot(handle);
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            const auto &value = result.Value();
            lua_createtable(state, 0, 8);
            PushCharacterUnsigned(state, value.tick);
            lua_setfield(state, -2, "tick");
            PushCharacterUnsigned(state, value.movement.sequence);
            lua_setfield(state, -2, "correlation");
            PushCharacterUnsigned(state, value.stateRevision);
            lua_setfield(state, -2, "revision");
            lua_pushnumber(state, value.transform.position.x);
            lua_setfield(state, -2, "x");
            lua_pushnumber(state, value.transform.position.y);
            lua_setfield(state, -2, "y");
            lua_pushnumber(state, value.transform.position.z);
            lua_setfield(state, -2, "z");
            lua_pushboolean(state, value.movement.grounded);
            lua_setfield(state, -2, "grounded");
            return 1;
        }

        /** @brief Uses only the host-selected principal; scripts cannot choose a different module or scene. */
        int Acquire(lua_State *state) {
            const auto &context = *static_cast<Context *>(lua_touserdata(state, lua_upvalueindex(1)));
            if (context.error)
                return Failure(state, *context.error);
            if (!context.scope)
                return Failure(state, MakeError(GameplayErrors::CharacterUnavailable));
            const auto &binding = context.scope->Binding();
            auto acquired = context.scope->Acquire(binding.moduleId, binding.scene, binding.sceneGeneration);
            if (acquired.HasError())
                return Failure(state, acquired.ErrorValue());
            Push(state, std::move(acquired).Value(), ClientType);
            luaL_getmetatable(state, ClientType);
            lua_createtable(state, 0, 5);
            for (const auto &[name, callback] : std::array<std::pair<const char *, lua_CFunction>, 5>{{{"identity", Guarded<Identity>},
                                                                                                       {"validate", Guarded<Validate>},
                                                                                                       {"controller", Guarded<Controller>},
                                                                                                       {"queue", Guarded<Queue>},
                                                                                                       {"snapshot", Guarded<Snapshot>}}}) {
                lua_pushcfunction(state, callback);
                lua_setfield(state, -2, name);
            }
            lua_setfield(state, -2, "__index");
            lua_pop(state, 1);
            return 1;
        }
    }  // namespace

    LuaCharacterScopeRegistry::~LuaCharacterScopeRegistry() {
        Revoke();
    }

    /** @copydoc LuaCharacterScopeRegistry::Retain */
    bool LuaCharacterScopeRegistry::Retain(const std::shared_ptr<const GameplayCharacterContext> &scope) {
        const auto slot = std::ranges::find_if(scopes_, [](const auto &item) {
            return item.expired();
        });
        if (slot == scopes_.end())
            return false;
        *slot = scope;
        return true;
    }

    /** @copydoc LuaCharacterScopeRegistry::Revoke */
    void LuaCharacterScopeRegistry::Revoke() noexcept {
        for (auto &item : scopes_) {
            if (const auto scope = item.lock())
                scope->Revoke();
            item.reset();
        }
    }

    /** @copydoc PushLuaCharacterContext */
    void PushLuaCharacterContext(lua_State *state, std::shared_ptr<const GameplayCharacterContext> scope,
                                 const std::optional<Error> &error) {
        lua_createtable(state, 0, 1);
        Push(state, Context{std::move(scope), error}, ContextType);
        lua_pushcclosure(state, Guarded<Acquire>, 1);
        lua_setfield(state, -2, "acquire");
    }
}  // namespace Horo::Gameplay::Detail
