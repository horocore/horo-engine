#include "LuaRuntimeUiBinding.h"
#include "LuaRuntimeUiTransport.h"

namespace Horo::Gameplay::Detail {
    using namespace Runtime::Ui;
    using namespace UiScript;

    namespace {
        /** @brief Borrows the exact locked host-issued holder for one synchronous observation. */
        const RuntimeUiClient *Client(lua_State *state) {
            const auto *holder = Transport<std::shared_ptr<const RuntimeUiClient>>(state, 1, ClientType);
            return holder ? holder->get() : nullptr;
        }

        /** @brief Missing owner has the same typed identity in native and script consumers. */
        int Missing(lua_State *state) {
            return Failure(state, MakeError(UiErrors::CapabilityUnavailable));
        }

        /** @brief Owned copied terminal payload survives a failed Lua projection and can be unpacked again. */
        struct TerminalRecord final {
            std::optional<RuntimeUiCommandResult> value;
        };

        static_assert(std::is_nothrow_move_assignable_v<std::optional<RuntimeUiCommandResult>>);

        /** @brief Projects a borrowed retained terminal record with POD/reference-only scratch across Lua allocation. */
        int ProjectTerminal(lua_State *state, const TerminalRecord &record) {
            const auto &value = *record.value;
            lua_pushinteger(state, static_cast<lua_Integer>(value.index()));
            if (const auto *screen = std::get_if<UiRouteOperationResult>(&value)) {
                lua_pushinteger(state, static_cast<int>(screen->outcome));
                lua_pushinteger(state, static_cast<int>(screen->rejection));
                PushUnsigned(state, screen->revision.Value());
                PushUnsigned(state, screen->operation.ownership.Value());
                PushUnsigned(state, screen->operation.sequence.Value());
                lua_pushinteger(state, static_cast<int>(screen->kind));
                if (screen->route)
                    Push(state, RouteType, *screen->route);
                else
                    lua_pushnil(state);
                return 8;
            }
            if (const auto *binding = std::get_if<UiBindingApplyResult>(&value)) {
                PushUnsigned(state, binding->revision.Value());
                lua_pushinteger(state, static_cast<lua_Integer>(binding->targetsChanged));
                lua_pushinteger(state, static_cast<lua_Integer>(binding->requiredUnavailable));
                PushUnsigned(state, binding->content.Value());
                return 5;
            }
            const auto &action = std::get<UiActionRequestId>(value);
            PushUnsigned(state, action.sequence.Value());
            PushUnsigned(state, action.ownership.Value());
            return 3;
        }

        /** @brief Projects every canonical result field without consuming the retained terminal transport. */
        int UnpackTerminal(lua_State *state) {
            const auto *record = Transport<TerminalRecord>(state, 1, TerminalType);
            if (!record || !record->value)
                return Invalid(state);
            if (const auto *error = std::get_if<Error>(&*record->value))
                return Failure(state, *error);
            return Protected(state, [record](lua_State *inner) {
                return ProjectTerminal(inner, *record);
            });
        }

        /** @brief Reserve all Lua output and method storage before consuming a native observation. */
        template <class T, lua_CFunction Unpack> T *ReserveObservation(lua_State *state, const char *name) {
            const T empty{};
            Protected(state, [name, &empty](lua_State *inner) {
                Push(inner, name, empty);
                luaL_getmetatable(inner, name);
                lua_getfield(inner, -1, "__index");
                if (lua_isnil(inner, -1)) {
                    lua_pop(inner, 1);
                    lua_newtable(inner);
                    lua_pushcfunction(inner, Boundary<Unpack>);
                    lua_setfield(inner, -2, "unpack");
                    lua_setfield(inner, -2, "__index");
                } else
                    lua_pop(inner, 1);
                lua_pop(inner, 1);
                return 1;
            });
            return static_cast<T *>(lua_touserdata(state, -1));
        }

        /** @brief Reserve typed output before native observation; projection allocation cannot lose an observed result. */
        int Poll(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            const auto *request = Transport<RuntimeUiRequestId>(state, 2, RequestType);
            if (!request)
                return Invalid(state);
            auto *output = ReserveObservation<TerminalRecord, UnpackTerminal>(state, TerminalType);
            auto result = client->Poll(*request);
            if (result.HasError()) {
                lua_pop(state, 1);
                return Failure(state, result.ErrorValue());
            }
            if (!result.Value()) {
                lua_pop(state, 1);
                lua_pushnil(state);
                return 1;
            }
            output->value = std::move(result).Value();
            return 1;
        }

        /** @brief Copied target event remains retryable if its Lua projection runs out of memory. */
        struct EventRecord final {
            std::optional<RuntimeUiEvent> value;
        };

        static_assert(std::is_nothrow_move_assignable_v<std::optional<RuntimeUiEvent>>);

        /** @brief Projects a borrowed retained event record with POD/reference-only scratch across Lua allocation. */
        int ProjectEvent(lua_State *state, const EventRecord &record) {
            Push(state, ElementType, record.value->target);
            lua_pushinteger(state, static_cast<int>(record.value->event.kind));
            PushUnsigned(state, record.value->event.sequence);
            lua_pushnumber(state, record.value->event.logicalPosition.x);
            lua_pushnumber(state, record.value->event.logicalPosition.y);
            lua_pushboolean(state, record.value->event.hasLogicalPosition);
            if (const auto &gesture = record.value->event.gesture) {
                lua_createtable(state, 8, 0);
                lua_pushinteger(state, static_cast<int>(gesture->kind));
                lua_rawseti(state, -2, 1);
                lua_pushinteger(state, gesture->pointer);
                lua_rawseti(state, -2, 2);
                Push(state, ElementType, gesture->source);
                lua_rawseti(state, -2, 3);
                lua_pushnumber(state, gesture->delta.x);
                lua_rawseti(state, -2, 4);
                lua_pushnumber(state, gesture->delta.y);
                lua_rawseti(state, -2, 5);
                lua_pushnumber(state, gesture->scale);
                lua_rawseti(state, -2, 6);
                lua_pushnumber(state, gesture->rotation);
                lua_rawseti(state, -2, 7);
                lua_pushboolean(state, gesture->accessible);
                lua_rawseti(state, -2, 8);
            } else
                lua_pushnil(state);
            return 7;
        }

        /** @brief Unpacks every canonical event field without consuming the retained copied event. */
        int UnpackEvent(lua_State *state) {
            const auto *record = Transport<EventRecord>(state, 1, EventType);
            if (!record || !record->value)
                return Invalid(state);
            return Protected(state, [record](lua_State *inner) {
                return ProjectEvent(inner, *record);
            });
        }

        /** @brief Reserve transport before removing a copied notification from the native mailbox. */
        int NextEvent(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            auto *output = ReserveObservation<EventRecord, UnpackEvent>(state, EventType);
            auto result = client->NextEvent();
            if (result.HasError()) {
                lua_pop(state, 1);
                return Failure(state, result.ErrorValue());
            }
            if (!result.Value()) {
                lua_pop(state, 1);
                lua_pushnil(state);
                return 1;
            }
            output->value = std::move(result).Value();
            return 1;
        }

    }  // namespace

    /** @copydoc InstallLuaRuntimeUiObservationMethods */
    void InstallLuaRuntimeUiObservationMethods(lua_State *state) {
        lua_pushcfunction(state, Boundary<Poll>);
        lua_setfield(state, -2, "poll");
        lua_pushcfunction(state, Boundary<NextEvent>);
        lua_setfield(state, -2, "next_event");
    }
}  // namespace Horo::Gameplay::Detail
