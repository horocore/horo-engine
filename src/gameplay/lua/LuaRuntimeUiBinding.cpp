#include "LuaRuntimeUiBinding.h"

#include "LuaRuntimeUiProtected.h"
#include "LuaRuntimeUiTransport.h"
#include "LuaRuntimeUiValues.h"

#include <algorithm>
#include <limits>
#include <memory>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    using namespace Runtime::Ui;
    using namespace UiScript;

    namespace {
        using Holder = std::shared_ptr<const RuntimeUiClient>;

        /** @brief Borrow exact userdata; every operation checks the same host client, never BehaviorContext. */
        const RuntimeUiClient *Client(lua_State *state) {
            const auto *holder = Transport<Holder>(state, 1, ClientType);
            return holder ? holder->get() : nullptr;
        }

        /** @brief Reads only a dense bounded raw tuple, with no author metamethod or unbounded key scan. */
        bool Dense(lua_State *state, int index, const std::size_t count) {
            index = lua_absindex(state, index);
            std::size_t keys{};
            lua_pushnil(state);
            while (lua_next(state, index)) {
                const bool validKey = lua_isinteger(state, -2) && lua_tointeger(state, -2) >= 1 &&
                                      static_cast<std::size_t>(lua_tointeger(state, -2)) <= count;
                if (validKey)
                    ++keys;
                const bool admitted = validKey && keys <= count;
                lua_pop(state, 1);
                if (!admitted) {
                    lua_pop(state, 1);
                    return false;
                }
            }
            return keys == count;
        }

        /** @brief Explicit missing consumer failure, matching native Runtime UI capability semantics. */
        int Missing(lua_State *state) {
            return Failure(state, MakeError(UiErrors::CapabilityUnavailable));
        }

        /** @brief Reserve the typed output before native admission, so Lua allocation failure cannot hide a queued request. */
        RuntimeUiRequestId *ReserveRequest(lua_State *state) {
            Protected(state, [](lua_State *inner) {
                Push(inner, RequestType, RuntimeUiRequestId{});
                return 1;
            });
            return static_cast<RuntimeUiRequestId *>(lua_touserdata(state, -1));
        }

        /** @brief Finish a preallocated request result; success performs no Lua operation that can allocate. */
        int Request(lua_State *state, RuntimeUiRequestId *output, const Result<RuntimeUiRequestId> &result) {
            if (result.HasError()) {
                lua_pop(state, 1);
                return Failure(state, result.ErrorValue());
            }
            *output = result.Value();
            return 1;
        }

        /** @brief Resolve a stable ID through the same canonical tree used by native callers. */
        int Find(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            auto bytes = ReadIdentity(state, 2);
            if (bytes.HasError())
                return Failure(state, bytes.ErrorValue());
            auto id = UiElementId::Create(bytes.Value());
            if (id.HasError())
                return Failure(state, id.ErrorValue());
            auto result = client->Find(id.Value());
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            return ProjectTransport(state, ElementType, result.Value());
        }

        /** @brief Copy a committed canonical binding value; no script callback becomes a provider getter. */
        int Read(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            auto bytes = ReadIdentity(state, 2);
            if (bytes.HasError())
                return Failure(state, bytes.ErrorValue());
            auto id = UiBindingId::Create(bytes.Value());
            if (id.HasError())
                return Failure(state, id.ErrorValue());
            auto value = client->Read(id.Value());
            if (value.HasError())
                return Failure(state, value.ErrorValue());
            PushBindingValue(state, std::move(value).Value());
            return 1;
        }

        /** @brief Capture a canonical guard instead of reconstructing stack internals in Lua. */
        int Guard(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            auto result = client->ScreenGuard();
            if (result.HasError())
                return Failure(state, result.ErrorValue());
            return ProjectTransport(state, GuardType, result.Value());
        }

        /** @brief Queue the closed route vocabulary and an optional opaque canonical guard. */
        int Screen(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            if (!lua_isinteger(state, 2))
                return Invalid(state);
            const auto kind = lua_tointeger(state, 2);
            if (kind < 0 || kind >= static_cast<int>(UiRouteOperationKind::Count))
                return Invalid(state);
            UiRouteOperationRequest request;
            request.kind = static_cast<UiRouteOperationKind>(kind);
            if (!lua_isnoneornil(state, 3)) {
                auto bytes = ReadIdentity(state, 3);
                if (bytes.HasError())
                    return Failure(state, bytes.ErrorValue());
                auto id = UiRouteId::Create(bytes.Value());
                if (id.HasError())
                    return Failure(state, id.ErrorValue());
                request.route = id.Value();
            }
            if (!lua_isnoneornil(state, 4)) {
                const auto *guard = Transport<UiRouteStackGuard>(state, 4, GuardType);
                if (!guard)
                    return Invalid(state);
                request.guard = *guard;
            }
            auto *output = ReserveRequest(state);
            return Request(state, output, client->Screen(request));
        }

        /** @brief Decode a dense bounded provider batch into an outer-owned detached update; never publish during Lua reads. */
        bool DecodeProviderChanges(lua_State *state, RuntimeUiProviderUpdate &update, const std::size_t count) {
            update.changes.reserve(count);
            bool decoded{};
            Protected(state, [&update, count, &decoded](lua_State *inner) {
                if (!Dense(inner, 8, count))
                    return 0;
                for (std::size_t i = 1; i <= count; ++i) {
                    lua_rawgeti(inner, 8, static_cast<lua_Integer>(i));
                    if (lua_type(inner, -1) != LUA_TTABLE || !Dense(inner, -1, 2))
                        return 0;
                    lua_rawgeti(inner, -1, 1);
                    lua_rawgeti(inner, -2, 2);
                    const auto *value = static_cast<UiBindingValue *>(luaL_testudata(inner, -1, ValueType));
                    if (!lua_isinteger(inner, -2) || lua_tointeger(inner, -2) < 0 || lua_tointeger(inner, -2) > UINT16_MAX || !value)
                        return 0;
                    update.changes.emplace_back(static_cast<std::uint16_t>(lua_tointeger(inner, -2)), *value);
                    lua_pop(inner, 3);
                }
                decoded = true;
                return 0;
            }, true);
            return decoded;
        }

        /** @brief Admit only this scope's explicitly permitted frozen provider and a dense bounded raw update list. */
        int Publish(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            if (!lua_isinteger(state, 2) || !lua_isinteger(state, 3) || !lua_isinteger(state, 4) || lua_type(state, 8) != LUA_TTABLE)
                return Invalid(state);
            const auto provider = lua_tointeger(state, 2);
            const auto major = lua_tointeger(state, 3);
            const auto minor = lua_tointeger(state, 4);
            std::uint64_t fingerprint{};
            std::uint64_t expected{};
            std::uint64_t revision{};
            if (provider < 1 || static_cast<std::size_t>(provider) > client->Grant().providers.size() || major <= 0 || major > UINT32_MAX ||
                minor < 0 || minor > UINT32_MAX || !ReadUnsigned(state, 5, fingerprint) || !ReadUnsigned(state, 6, expected) ||
                !ReadUnsigned(state, 7, revision))
                return Invalid(state);
            auto schema = UiBindingSchemaVersion::Create(static_cast<std::uint32_t>(major), static_cast<std::uint32_t>(minor), fingerprint);
            auto oldRevision = UiBindingSnapshotRevision::Create(expected);
            auto newRevision = UiBindingSnapshotRevision::Create(revision);
            if (schema.HasError())
                return Failure(state, schema.ErrorValue());
            if (oldRevision.HasError())
                return Failure(state, oldRevision.ErrorValue());
            if (newRevision.HasError())
                return Failure(state, newRevision.ErrorValue());
            const auto count = lua_rawlen(state, 8);
            if (!count || count > 16)
                return Invalid(state);
            RuntimeUiProviderUpdate update{client->Grant().providers[static_cast<std::size_t>(provider - 1)],
                                           schema.Value(),
                                           oldRevision.Value(),
                                           newRevision.Value(),
                                           {}};
            if (!DecodeProviderChanges(state, update, count))
                return Invalid(state);
            auto *output = ReserveRequest(state);
            return Request(state, output, client->Publish(std::move(update)));
        }

        /** @brief Produce source evidence from an opaque resident element and an exact lossless revision. */
        int Source(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            const auto *element = Transport<UiElementHandle>(state, 2, ElementType);
            std::uint64_t revision{};
            if (!element || !ReadUnsigned(state, 3, revision))
                return Invalid(state);
            auto interaction = UiInteractionRevision::Create(revision);
            if (interaction.HasError())
                return Failure(state, interaction.ErrorValue());
            auto source = client->Source(*element, interaction.Value());
            if (source.HasError())
                return Failure(state, source.ErrorValue());
            return ProjectTransport(state, SourceType, source.Value());
        }

        /** @brief Copy canonical typed action values while all nontrivial payload storage remains outside the Lua jump frame. */
        bool DecodeActionValues(lua_State *state, std::array<UiActionValue, MaximumUiActionArguments> &values, const std::size_t count) {
            bool decoded{};
            Protected(state, [&values, count, &decoded](lua_State *inner) {
                if (!Dense(inner, 6, count))
                    return 0;
                for (std::size_t i = 0; i < count; ++i) {
                    lua_rawgeti(inner, 6, static_cast<lua_Integer>(i + 1));
                    const auto *value = static_cast<UiActionValue *>(luaL_testudata(inner, -1, ActionValueType));
                    if (!value)
                        return 0;
                    values[i] = *value;
                    lua_pop(inner, 1);
                }
                decoded = true;
                return 0;
            }, true);
            return decoded;
        }

        /** @brief Inert copied command selection evidence; contains no provider, renderer or VM owner. */
        struct ActionSelection final {
            UiActionCommandKind kind;
            lua_Integer operation;
            UiActionId id;
            UiElementHandle element;
            std::size_t count;
        };

        /** @brief Validate the chosen canonical command family after bounded payload decoding and before native admission. */
        Result<UiActionCommand> BuildActionCommand(const ActionSelection &input, const UiActionPayload &payload) {
            UiActionCommand command;
            switch (input.kind) {
                case UiActionCommandKind::Button:
                    command = UiButtonActionCommand{input.id, payload};
                    break;
                case UiActionCommandKind::Form:
                    if (input.operation < 0 || input.operation >= static_cast<int>(UiFormActionKind::Count))
                        return Result<UiActionCommand>::Failure(MakeError(UiErrors::CapabilityUnsupported));
                    command = UiFormActionCommand{input.id, static_cast<UiFormActionKind>(input.operation), payload};
                    break;
                case UiActionCommandKind::Route:
                    if (input.operation < 0 || input.operation >= static_cast<int>(UiRouteActionKind::Count))
                        return Result<UiActionCommand>::Failure(MakeError(UiErrors::CapabilityUnsupported));
                    command = UiRouteActionCommand{input.id, static_cast<UiRouteActionKind>(input.operation), payload};
                    break;
                case UiActionCommandKind::Gameplay:
                    command = UiGameplayActionCommand{input.id, payload};
                    break;
                case UiActionCommandKind::Navigation:
                    if (input.count || input.operation < 0 || input.operation >= static_cast<int>(UiNavigationDirection::Count))
                        return Result<UiActionCommand>::Failure(MakeError(UiErrors::CapabilityUnsupported));
                    command = UiNavigationCommand{static_cast<UiNavigationDirection>(input.operation), input.element};
                    break;
                default:
                    return Result<UiActionCommand>::Failure(MakeError(UiErrors::CapabilityUnsupported));
            }
            return Result<UiActionCommand>::Success(std::move(command));
        }

        /** @brief Marshal all canonical command families and fixed-capacity action arguments. */
        int Action(lua_State *state) {
            const auto *client = Client(state);
            if (!client)
                return Missing(state);
            const auto *source = Transport<UiActionSource>(state, 2, SourceType);
            if (!source || !lua_isinteger(state, 3) || !lua_isinteger(state, 5) || lua_type(state, 6) != LUA_TTABLE)
                return Invalid(state);
            const auto kind = lua_tointeger(state, 3);
            const auto operation = lua_tointeger(state, 5);
            if (kind < 0 || kind >= static_cast<int>(UiActionCommandKind::Count))
                return Invalid(state);
            auto bytes = ReadIdentity(state, 4);
            if (bytes.HasError())
                return Failure(state, bytes.ErrorValue());
            auto id = UiActionId::Create(bytes.Value());
            if (id.HasError())
                return Failure(state, id.ErrorValue());
            const auto count = lua_rawlen(state, 6);
            if (count > MaximumUiActionArguments)
                return Invalid(state);
            std::array<UiActionValue, MaximumUiActionArguments> values;
            if (!DecodeActionValues(state, values, count))
                return Invalid(state);
            auto payload = UiActionPayload::Create(std::span{values.data(), count});
            if (payload.HasError())
                return Failure(state, payload.ErrorValue());
            auto command = BuildActionCommand({static_cast<UiActionCommandKind>(kind), operation, id.Value(), source->element, count},
                                              payload.Value());
            if (command.HasError())
                return Failure(state, command.ErrorValue());
            auto *output = ReserveRequest(state);
            return Request(state, output, client->Action(*source, std::move(command).Value()));
        }

    }  // namespace

    /** @copydoc LuaRuntimeUiScopes::Bind */
    Result<std::shared_ptr<const RuntimeUiClient>> LuaRuntimeUiScopes::Bind(const std::shared_ptr<const RuntimeUiClient> &parent) {
        using Output = Result<std::shared_ptr<const RuntimeUiClient>>;
        if (!parent)
            return Output::Success({});
        const auto slot = std::ranges::find_if(scopes_, [](const auto &entry) {
            return entry.expired();
        });
        if (slot == scopes_.end())
            return Output::Failure(MakeError(UiErrors::CapacityExceeded));
        auto child = parent->Derive();
        if (child.HasError())
            return child;
        *slot = child.Value();
        return child;
    }

    /** @copydoc LuaRuntimeUiScopes::Revoke */
    void LuaRuntimeUiScopes::Revoke() const noexcept {
        for (const auto &entry : scopes_)
            if (auto client = entry.lock())
                client->Revoke();
    }

    /** @copydoc PushLuaRuntimeUi */
    void PushLuaRuntimeUi(lua_State *state, const Holder &client) {
        static const char ClientRegistryKey{};
        if (lua_rawgetp(state, LUA_REGISTRYINDEX, &ClientRegistryKey) != LUA_TNIL)
            return;
        lua_pop(state, 1);
        Push(state, ClientType, client);
        luaL_getmetatable(state, ClientType);
        lua_newtable(state);
        const std::array<luaL_Reg, 8> functions = {{{"find", Boundary<Find>},
                                                    {"read", Boundary<Read>},
                                                    {"screen_guard", Boundary<Guard>},
                                                    {"screen", Boundary<Screen>},
                                                    {"publish", Boundary<Publish>},
                                                    {"source", Boundary<Source>},
                                                    {"action", Boundary<Action>},
                                                    {nullptr, nullptr}}};
        luaL_setfuncs(state, functions.data(), 0);
        InstallLuaRuntimeUiObservationMethods(state);
        InstallValueFactories(state);
        lua_setfield(state, -2, "__index");
        lua_pop(state, 1);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &ClientRegistryKey);
    }
}  // namespace Horo::Gameplay::Detail
