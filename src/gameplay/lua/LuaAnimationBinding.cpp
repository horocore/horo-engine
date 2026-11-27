#include "LuaAnimationBinding.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace A = Animation;

    LuaAnimationCallback::LuaAnimationCallback(LuaAnimationCallbackState &state) noexcept : state_(state) {
        if (state_.epoch != std::numeric_limits<std::uint64_t>::max()) {
            ++state_.epoch;
            state_.active = true;
        }
    }

    LuaAnimationCallback::~LuaAnimationCallback() {
        state_.active = false;
    }

    namespace {
        constexpr auto ScopeMetatable = "horo.gameplay.animation.scope";
        constexpr auto ActionMetatable = "horo.gameplay.animation.action";
        constexpr std::string_view Invalid = "animation.handle.malformed";
        constexpr std::string_view Expired = "animation.callback.expired";

        struct Scope final {
            std::shared_ptr<const GameplayAnimationContext> grant;
            GameplayEntityRef entity;
            const LuaAnimationCallbackState *callback;
            std::uint64_t epoch;
        };

        template <typename Value> struct Reply final {
            std::optional<Value> value;
            std::array<char, 96> code{};
            std::size_t codeSize{};

            void Fail(std::string_view text) noexcept {
                codeSize = std::min(text.size(), code.size());
                text.copy(code.data(), codeSize);
            }
        };

        /** @brief Copies error codes before a fallible Lua push can unwind the VM stack. */
        template <typename T> Reply<T> CopyReply(const Result<T> &result) {
            Reply<T> reply;
            if (result.HasValue())
                reply.value = result.Value();
            else
                reply.Fail(result.ErrorValue().code.Value());
            return reply;
        }

        Reply<bool> CopyReply(const Result<void> &result) {
            Reply<bool> reply;
            if (result.HasValue())
                reply.value = true;
            else
                reply.Fail(result.ErrorValue().code.Value());
            return reply;
        }

        /** @brief Resolves the bound self actor only while the issuing callback still owns admission. */
        template <typename Operation> auto Invoke(lua_State *state, Operation operation) {
            using ReplyType = decltype(CopyReply(
                operation(std::declval<const GameplayAnimationContext &>(), std::declval<const GameplayAnimationSnapshot &>())));
            const auto &scope = *static_cast<Scope *>(lua_touserdata(state, lua_upvalueindex(1)));
            ReplyType failed;
            if (!scope.callback->active || scope.epoch != scope.callback->epoch) {
                failed.Fail(Expired);
                return failed;
            }
            const auto observed = scope.grant->Observe(scope.entity);
            if (observed.HasError()) {
                failed.Fail(observed.ErrorValue().code.Value());
                return failed;
            }
            return CopyReply(operation(*scope.grant, observed.Value()));
        }

        int Error(lua_State *state, const std::string_view code) {
            lua_pushnil(state);
            lua_pushlstring(state, code.data(), code.size());
            return 2;
        }

        /** @brief Publishes a trivially destructible reply after all engine-owned results have been released. */
        template <typename T, typename Push> int Send(lua_State *state, const Reply<T> &reply, Push push) {
            static_assert(std::is_trivially_destructible_v<Reply<T>>);
            if (!reply.value.has_value())
                return Error(state, {reply.code.data(), reply.codeSize});
            push(state, *reply.value);
            return 1;
        }

        /** @brief Reads positive Lua integers or exact decimal strings, including IDs beyond signed 64-bit range. */
        std::uint64_t ReadId(lua_State *state, const int index) {
            if (lua_isinteger(state, index)) {
                const auto value = lua_tointeger(state, index);
                return value > 0 ? static_cast<std::uint64_t>(value) : 0;
            }
            if (lua_type(state, index) != LUA_TSTRING)
                return 0;
            std::size_t size{};
            const char *text = lua_tolstring(state, index, &size);
            std::uint64_t value{};
            const auto end = text + size;
            const auto parsed = std::from_chars(text, end, value);
            return parsed.ec == std::errc{} && parsed.ptr == end ? value : 0;
        }

        void PushId(lua_State *state, const std::uint64_t value) {
            std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> text;
            const auto end = std::to_chars(text.data(), text.data() + text.size(), value).ptr;
            lua_pushlstring(state, text.data(), static_cast<std::size_t>(end - text.data()));
        }

        void IdField(lua_State *state, const char *name, const std::uint64_t value) {
            PushId(state, value);
            lua_setfield(state, -2, name);
        }

        void NumberField(lua_State *state, const char *name, const lua_Number value) {
            lua_pushnumber(state, value);
            lua_setfield(state, -2, name);
        }

        void PushVector(lua_State *state, const Math::Vec3 value) {
            lua_createtable(state, 0, 3);
            NumberField(state, "x", value.x);
            NumberField(state, "y", value.y);
            NumberField(state, "z", value.z);
        }

        void PushDelta(lua_State *state, const char *name, const A::AnimationRootMotionDelta &delta) {
            lua_createtable(state, 0, 2);
            PushVector(state, delta.translation);
            lua_setfield(state, -2, "translation");
            lua_createtable(state, 0, 4);
            NumberField(state, "x", delta.rotation.x);
            NumberField(state, "y", delta.rotation.y);
            NumberField(state, "z", delta.rotation.z);
            NumberField(state, "w", delta.rotation.w);
            lua_setfield(state, -2, "rotation");
            lua_setfield(state, -2, name);
        }

        void PushSnapshot(lua_State *state, const GameplayAnimationSnapshot &snapshot) {
            lua_createtable(state, 0, 3);
            NumberField(state, "state", static_cast<lua_Number>(snapshot.state));
            lua_pushboolean(state, snapshot.current.IsValid());
            lua_setfield(state, -2, "has_pose");
            if (const auto &motion = snapshot.rootMotion; motion.has_value()) {
                lua_createtable(state, 0, 5);
                IdField(state, "tick", motion->request.tick.Value());
                NumberField(state, "disposition", static_cast<lua_Number>(motion->disposition));
                PushDelta(state, "requested", motion->requested);
                PushDelta(state, "applied", motion->applied);
                PushDelta(state, "remaining", motion->remaining);
                lua_setfield(state, -2, "root_motion");
            }
        }

        int Snapshot(lua_State *state) {
            return Send(state,
                        Invoke(state,
                               [](const auto &, const auto &snapshot) {
                return Result<GameplayAnimationSnapshot>::Success(snapshot);
            }),
                        PushSnapshot);
        }

        std::optional<A::GraphParameterValue> ReadParameter(lua_State *state, const int index) {
            if (lua_isboolean(state, index))
                return lua_toboolean(state, index) != 0;
            if (lua_isinteger(state, index)) {
                const auto value = lua_tointeger(state, index);
                if (value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max())
                    return static_cast<std::int32_t>(value);
                return std::nullopt;
            }
            if (lua_type(state, index) != LUA_TNUMBER)
                return std::nullopt;
            const auto value = lua_tonumber(state, index);
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                return std::nullopt;
            return static_cast<float>(value);
        }

        int SetParameter(lua_State *state) {
            const auto id = ReadId(state, 1);
            const auto value = ReadParameter(state, 2);
            if (id == 0 || !value.has_value())
                return Error(state, Invalid);
            return Send(state,
                        Invoke(state,
                               [id, &value](const auto &grant, const auto &snapshot) {
                return grant.QueueParameter(snapshot.actor, {A::GraphParameterId::Create(id).Value(), *value});
            }),
                        lua_pushboolean);
        }

        int Trigger(lua_State *state) {
            const auto id = ReadId(state, 1);
            if (id == 0)
                return Error(state, Invalid);
            return Send(state,
                        Invoke(state,
                               [id](const auto &grant, const auto &snapshot) {
                return grant.QueueTrigger(snapshot.actor, A::GraphParameterId::Create(id).Value());
            }),
                        lua_pushboolean);
        }

        int QueueAction(lua_State *state) {
            const auto id = ReadId(state, 1);
            const auto section = ReadId(state, 2);
            if (id == 0 || (!lua_isnoneornil(state, 2) && section == 0))
                return Error(state, Invalid);
            auto *handle = static_cast<A::GraphActionHandle *>(lua_newuserdatauv(state, sizeof(A::GraphActionHandle), 0));
            luaL_setmetatable(state, ActionMetatable);
            const auto reply = Invoke(state, [id, section](const auto &grant, const auto &snapshot) {
                GameplayAnimationAction request{.action = A::GraphActionId::Create(id).Value()};
                if (section != 0)
                    request.startSection = A::GraphActionSectionId::Create(section).Value();
                return grant.QueueAction(snapshot.actor, request);
            });
            if (!reply.value.has_value())
                return Error(state, {reply.code.data(), reply.codeSize});
            std::construct_at(handle, *reply.value);
            return 1;
        }

        /** @brief Runs an action control only for an opaque Lua handle issued for the bound actor. */
        template <auto Method> int Action(lua_State *state) {
            const auto *handle = static_cast<const A::GraphActionHandle *>(luaL_testudata(state, 1, ActionMetatable));
            if (!handle)
                return Error(state, Invalid);
            const auto reply = Invoke(state, [handle](const auto &grant, const auto &snapshot) {
                return (grant.*Method)(snapshot.actor, *handle);
            });
            return Send(state, reply, [](lua_State *lua, const auto &value) {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, bool>) {
                    lua_pushboolean(lua, value);
                } else {
                    lua_createtable(lua, 0, 5);
                    IdField(lua, "section", value.section.Value());
                    NumberField(lua, "phase", static_cast<lua_Number>(value.phase));
                    NumberField(lua, "reason", static_cast<lua_Number>(value.reason));
                    NumberField(lua, "weight", value.weight);
                    IdField(lua, "terminal_tick", value.terminalTick.Value());
                }
            });
        }

        int CancelAction(lua_State *state) {
            const auto *handle = static_cast<const A::GraphActionHandle *>(luaL_testudata(state, 1, ActionMetatable));
            if (!handle || (!lua_isnoneornil(state, 2) && !lua_isboolean(state, 2)))
                return Error(state, Invalid);
            const auto policy = lua_toboolean(state, 2) ? A::GraphActionStopPolicy::Cut : A::GraphActionStopPolicy::BlendOut;
            return Send(state,
                        Invoke(state,
                               [handle, policy](const auto &grant, const auto &snapshot) {
                return grant.CancelAction(snapshot.actor, *handle, policy);
            }),
                        lua_pushboolean);
        }

        int Socket(lua_State *state) {
            const auto id = ReadId(state, 1);
            if (id == 0)
                return Error(state, Invalid);
            return Send(state,
                        Invoke(state,
                               [id](const auto &grant, const auto &snapshot) {
                return grant.SocketModelTransform(snapshot.actor, snapshot.current, A::SkeletonSocketId::Create(id).Value());
            }),
                        [](lua_State *lua, const auto &matrix) {
                lua_createtable(lua, static_cast<int>(matrix.values.size()), 0);
                for (std::size_t i = 0; i < matrix.values.size(); ++i) {
                    lua_pushnumber(lua, matrix.values[i]);
                    lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
                }
            });
        }

        void PushParameter(lua_State *state, const A::GraphParameterInput &parameter) {
            lua_createtable(state, 0, 2);
            IdField(state, "id", parameter.id.Value());
            std::visit([state](const auto value) {
                using Type = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, bool>)
                    lua_pushboolean(state, value);
                else if constexpr (std::is_same_v<Type, std::int32_t>)
                    lua_pushinteger(state, value);
                else
                    lua_pushnumber(state, value);
            }, parameter.value);
            lua_setfield(state, -2, "value");
        }

        void PushStateEvent(lua_State *state, const A::GraphStateEvent &event) {
            lua_createtable(state, 0, 6);
            IdField(state, "tick", event.tick.Value());
            IdField(state, "state", event.state.Value());
            IdField(state, "transition", event.transition.Value());
            IdField(state, "activation", event.activation);
            NumberField(state, "kind", static_cast<lua_Number>(event.kind));
            NumberField(state, "ordinal", event.ordinal);
        }

        /** @brief Pushes a stable asset ID with fixed UUID scratch storage. */
        void PushAsset(lua_State *state, const Assets::AssetId &asset) {
            constexpr std::string_view hex = "0123456789abcdef";
            std::array<char, 36> text;
            std::size_t output{};
            for (std::size_t index = 0; index < asset.Bytes().size(); ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                    text[output++] = '-';
                const auto byte = asset.Bytes()[index];
                text[output++] = hex[byte >> 4U];
                text[output++] = hex[byte & 0xFU];
            }
            lua_pushlstring(state, text.data(), text.size());
        }

        void PushPayload(lua_State *state, const A::AnimationEventPayload &payload) {
            lua_createtable(state, 0, static_cast<int>(payload.count));
            for (const auto &field : std::span{payload.fields}.first(payload.count)) {
                PushId(state, field.id.Value());
                std::visit([state](const auto &value) {
                    using Type = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<Type, bool>)
                        lua_pushboolean(state, value);
                    else if constexpr (std::is_same_v<Type, std::int64_t>)
                        lua_pushinteger(state, value);
                    else if constexpr (std::is_same_v<Type, std::uint64_t>)
                        PushId(state, value);
                    else if constexpr (std::is_same_v<Type, Math::Vec3>)
                        PushVector(state, value);
                    else if constexpr (std::is_same_v<Type, Assets::AssetId>)
                        PushAsset(state, value);
                    else
                        lua_pushnumber(state, value);
                }, field.value);
                lua_settable(state, -3);
            }
        }

        void PushClipEvent(lua_State *state, const A::AnimationEventOccurrence &event) {
            lua_createtable(state, 0, 8);
            IdField(state, "tick", event.id.tick.Value());
            IdField(state, "type", event.type.Value());
            IdField(state, "marker", event.id.marker.Value());
            IdField(state, "serial", event.id.serial);
            NumberField(state, "phase", static_cast<lua_Number>(event.id.phase));
            NumberField(state, "reason", static_cast<lua_Number>(event.reason));
            NumberField(state, "schema_revision", event.schemaRevision);
            PushPayload(state, event.payload);
            lua_setfield(state, -2, "payload");
        }

        /** @brief Copies a complete bounded block into memory charged to the Lua VM. */
        template <typename Value, auto Method, auto Push, std::size_t Maximum> int Block(lua_State *state) {
            static_assert(std::is_trivially_destructible_v<Value>);
            if (!lua_isinteger(state, 1))
                return Error(state, Invalid);
            const auto requested = lua_tointeger(state, 1);
            if (requested < 0 || static_cast<std::uint64_t>(requested) > Maximum)
                return Error(state, Invalid);
            const auto count = static_cast<std::uint32_t>(requested);
            auto *storage = static_cast<Value *>(lua_newuserdatauv(state, sizeof(Value) * count, 0));
            std::uninitialized_value_construct_n(storage, count);
            return Send(state,
                        Invoke(state,
                               [storage, count](const auto &grant, const auto &snapshot) -> Result<std::uint32_t> {
                const auto copied = (grant.*Method)(snapshot.actor, snapshot.current, std::span<Value>{storage, count});
                if constexpr (std::is_same_v<decltype(copied), const Result<void>>)
                    return copied.HasValue() ? Result<std::uint32_t>::Success(count) : Result<std::uint32_t>::Failure(copied.ErrorValue());
                else
                    return copied;
            }),
                        [storage](lua_State *lua, const std::uint32_t size) {
                lua_createtable(lua, static_cast<int>(size), 0);
                for (std::uint32_t i = 0; i < size; ++i) {
                    Push(lua, storage[i]);
                    lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
                }
            });
        }

        int Destroy(lua_State *state) {
            std::destroy_at(static_cast<Scope *>(lua_touserdata(state, 1)));
            return 0;
        }
    }  // namespace

    /** @copydoc PushLuaAnimationContext */
    void PushLuaAnimationContext(lua_State *state, const std::shared_ptr<const GameplayAnimationContext> &context,
                                 const GameplayEntityRef entity, const LuaAnimationCallbackState &callback) {
        if (!context || !callback.active) {
            lua_pushnil(state);
            return;
        }
        if (luaL_newmetatable(state, ActionMetatable)) {
            lua_pushboolean(state, false);
            lua_setfield(state, -2, "__metatable");
        }
        lua_pop(state, 1);

        constexpr luaL_Reg operations[]{
            {"snapshot", Snapshot},
            {"set_parameter", SetParameter},
            {"trigger", Trigger},
            {"queue_action", QueueAction},
            {"action", Action<&GameplayAnimationContext::ObserveAction>},
            {"cancel_action", CancelAction},
            {"release_action", Action<&GameplayAnimationContext::ReleaseAction>},
            {"socket", Socket},
            {"parameters", Block<A::GraphParameterInput, &GameplayAnimationContext::CopyParameters, PushParameter,
                                 A::AnimationGraphHardLimits::Parameters>},
            {"state_events", Block<A::GraphStateEvent, &GameplayAnimationContext::CopyStateEvents, PushStateEvent,
                                   A::AnimationGraphHardLimits::StateEvents>},
            {"clip_events", Block<A::AnimationEventOccurrence, &GameplayAnimationContext::CopyClipEvents, PushClipEvent,
                                  A::AnimationNotifyHardLimits::OccurrencesPerTick>},
            {nullptr, nullptr},
        };
        lua_newtable(state);
        if (luaL_newmetatable(state, ScopeMetatable)) {
            lua_pushboolean(state, false);
            lua_setfield(state, -2, "__metatable");
            lua_pushcfunction(state, Destroy);
            lua_setfield(state, -2, "__gc");
        }
        auto *scope = static_cast<Scope *>(lua_newuserdatauv(state, sizeof(Scope), 0));
        std::construct_at(scope, Scope{context, entity, &callback, callback.epoch});
        lua_pushvalue(state, -2);
        lua_setmetatable(state, -2);
        lua_remove(state, -2);
        luaL_setfuncs(state, operations, 1);
    }
}  // namespace Horo::Gameplay::Detail
