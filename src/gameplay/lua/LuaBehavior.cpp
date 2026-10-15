#include "Horo/Gameplay/LuaBehavior.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Physics/CharacterErrors.h"
#include "LuaBehaviorMetadata.h"
#include "LuaBehaviorSandbox.h"
#include "LuaCharacterBinding.h"
#include "LuaNavigationBinding.h"
#include "LuaPhysicsBinding.h"
#include "LuaReplicationDeclaration.h"
#include "LuaRuntimeUiBinding.h"
#include "LuaRuntimeUiProtected.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace Horo::Gameplay {
    namespace {
        using Detail::BudgetAllocate;
        using Detail::LuaBudget;

        void InstructionLimitHook(lua_State *state, lua_Debug *) {
            luaL_error(state, "Lua behavior instruction budget exceeded");
        }

        [[nodiscard]] Result<void> LuaFailure(const std::string &message) {
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent, message));
        }

        struct ParsedProgram {
            BehaviorDescriptor descriptor;
            std::optional<GameplayReplicationRegistration> replication;
        };

        [[nodiscard]] Result<ParsedProgram> ParseProgram(std::string_view source, const BehaviorTypeId &canonicalTypeId,
                                                         const std::string &sourceName, const LuaBehaviorLimits limits,
                                                         const Network::ReplicationSchemaId schema, const ModuleId &moduleId) {
            LuaBudget budget{0, limits.maximumMemoryBytes};
            lua_State *state = lua_newstate(BudgetAllocate, &budget);
            if (state == nullptr)
                return Result<ParsedProgram>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent, "Unable to create Lua VM."));
            Detail::OpenLuaBehaviorSandbox(state);
            lua_sethook(state, InstructionLimitHook, LUA_MASKCOUNT, static_cast<int>(limits.maximumInstructionsPerCallback));
            if (const int loaded = luaL_loadbufferx(state, source.data(), source.size(), sourceName.c_str(), "t");
                loaded != LUA_OK || lua_pcall(state, 0, 1, 0) != LUA_OK) {
                const std::string message = lua_isstring(state, -1) ? lua_tostring(state, -1) : "Lua script compilation failed.";
                lua_close(state);
                return Result<ParsedProgram>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent, message));
            }
            auto descriptor = Detail::ReadLuaBehaviorDescriptor(state, canonicalTypeId);
            if (descriptor.HasError()) {
                lua_close(state);
                return Result<ParsedProgram>::Failure(descriptor.ErrorValue());
            }
            auto replication = Detail::ReadLuaReplicationDeclaration(state, canonicalTypeId, schema, moduleId);
            lua_close(state);
            if (replication.HasError())
                return Result<ParsedProgram>::Failure(replication.ErrorValue());
            return Result<ParsedProgram>::Success({std::move(descriptor).Value(), std::move(replication).Value()});
        }

    }  // namespace

    struct LuaBehaviorProgram::Impl {
        BehaviorDescriptor descriptor;
        std::optional<GameplayReplicationRegistration> replication;
        std::unique_ptr<ReplicationRegistrationRegistry> replicationRegistry;
        std::string source;
        std::string sourceName;
        LuaBehaviorLimits limits;
        std::uint64_t revision{1};
        Detail::LuaRuntimeUiScopes uiScopes;
        Detail::LuaCharacterScopeRegistry characterScopes;
    };

    class LuaBehaviorInstance final : public IBehaviorInstance {
    public:
        explicit LuaBehaviorInstance(LuaBehaviorProgram &program) : program_(&program) {}

        LuaBehaviorInstance(const LuaBehaviorInstance &) = delete;
        LuaBehaviorInstance &operator=(const LuaBehaviorInstance &) = delete;
        LuaBehaviorInstance(LuaBehaviorInstance &&) = delete;
        LuaBehaviorInstance &operator=(LuaBehaviorInstance &&) = delete;

        ~LuaBehaviorInstance() override {
            Close();
        }

        void OnCreate(BehaviorContext &context) override {
            Call("on_create", context);
        }

        void OnEnable(BehaviorContext &context) override {
            Call("on_enable", context);
        }

        void OnStart(BehaviorContext &context) override {
            Call("on_start", context);
        }

        void OnInputAction(BehaviorContext &context, const GameplayInputAction &action) override {
            Call("on_input_action", context, std::nullopt, &action);
        }

        void OnEvent(BehaviorContext &context, const GameplayEvent &event) override {
            Call("on_event", context, std::nullopt, nullptr, &event);
        }

        void OnFixedUpdate(BehaviorContext &context, const FixedDeltaTime delta) override {
            Call("on_fixed_update", context, delta.seconds);
        }

        void OnPresentationUpdate(BehaviorContext &context, const FrameDeltaTime delta) override {
            Call("on_presentation_update", context, delta.seconds);
        }

        void OnDisable(BehaviorContext &context) override {
            Call("on_disable", context);
        }

        void OnDestroy(BehaviorContext &context) override {
            Call("on_destroy", context);
        }

    private:
        struct Vm {
            LuaBudget budget;
            lua_State *state{};
            int behaviorRef{LUA_NOREF};
            std::uint64_t revision{};
            std::optional<CancellationSource> navigationLifetime;
            CancellationToken navigationToken;
            std::shared_ptr<const Runtime::Ui::RuntimeUiClient> ui;
            bool uiBound{};
        };

        static BehaviorContext &Context(lua_State *state) {
            return *static_cast<BehaviorContext *>(lua_touserdata(state, lua_upvalueindex(1)));
        }

        static int Position(lua_State *state) {
            auto transform = Context(state).LocalTransform();
            if (transform.HasError())
                return luaL_error(state, "entity transform is unavailable");
            lua_pushnumber(state, transform.Value().translation.x);
            lua_pushnumber(state, transform.Value().translation.y);
            lua_pushnumber(state, transform.Value().translation.z);
            return 3;
        }

        static int SetPosition(lua_State *state) {
            auto transform = Context(state).LocalTransform();
            if (transform.HasError())
                return luaL_error(state, "entity transform is unavailable");
            Math::Transform changed = transform.Value();
            changed.translation = {static_cast<float>(luaL_checknumber(state, 1)), static_cast<float>(luaL_checknumber(state, 2)),
                                   static_cast<float>(luaL_checknumber(state, 3))};
            if (Context(state).SetLocalTransform(changed).HasError())
                return luaL_error(state, "transform mutation was rejected");
            return 0;
        }

        static int Action(lua_State *state) {
            const std::string_view requested = luaL_checkstring(state, 1);
            for (const GameplayInputAction &action : Context(state).InputActions()) {
                if (action.action.Value() != requested)
                    continue;
                lua_pushnumber(state, action.x);
                lua_pushnumber(state, action.y);
                lua_pushboolean(state, action.down);
                lua_pushboolean(state, action.pressed);
                lua_pushboolean(state, action.released);
                return 5;
            }
            lua_pushnumber(state, 0);
            lua_pushnumber(state, 0);
            lua_pushboolean(state, false);
            lua_pushboolean(state, false);
            lua_pushboolean(state, false);
            return 5;
        }

        static int Publish(lua_State *state) {
            if (GameplayEvent event{GameplayEventTypeId{luaL_checkstring(state, 1)}, 1, std::nullopt, {}};
                Context(state).Publish(std::move(event)).HasError())
                return luaL_error(state, "event publication was rejected");
            return 0;
        }

        /** @brief Pushes one typed scalar field consistently for callback lookup and event delivery. */
        static void PushFieldValue(lua_State *state, const BehaviorFieldValue &value) {
            std::visit([state]<typename T>(const T &value) {
                if constexpr (std::is_same_v<T, bool>)
                    lua_pushboolean(state, value);
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    lua_pushinteger(state, static_cast<lua_Integer>(value));
                else if constexpr (std::is_same_v<T, double>)
                    lua_pushnumber(state, value);
                else if constexpr (std::is_same_v<T, std::string>)
                    lua_pushlstring(state, value.data(), value.size());
                else
                    lua_pushnil(state);
            }, value);
        }

        static int Field(lua_State *state) {
            const std::string_view name = luaL_checkstring(state, 1);
            for (const BehaviorField &field : Context(state).Fields()) {
                if (field.name != name)
                    continue;
                PushFieldValue(state, field.value);
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }

        static int LogInfo(lua_State *state) {
            LOG_INFO("gameplay.lua", "%s", luaL_checkstring(state, 1));
            return 0;
        }

        template <lua_CFunction Callback> static void Function(lua_State *state, BehaviorContext &context, const char *name) {
            lua_pushlightuserdata(state, &context);
            lua_pushcclosure(state, Callback, 1);
            lua_setfield(state, -2, name);
        }

        /** @brief Binds one script incarnation to an independently revocable Character child without retaining BehaviorContext. */
        void BindCharacter(const BehaviorContext &context) {
            if (character_ || characterError_)
                return;
            const auto parent = context.CharacterContext();
            if (!parent)
                return;
            auto child = parent->Derive();
            if (child.HasError()) {
                characterError_ = child.ErrorValue();
                return;
            }
            if (!program_->impl_->characterScopes.Retain(child.Value())) {
                child.Value()->Revoke();
                characterError_ = MakeError(Character::CharacterErrors::CapacityExceeded);
                return;
            }
            character_ = std::move(child).Value();
        }

        void PushContext(lua_State *state, BehaviorContext &context, const std::shared_ptr<const GameplayPhysicsContext> &physics,
                         const std::shared_ptr<const GameplayNavigationContext> &navigation) const {
            lua_newtable(state);
            Detail::PushLuaNavigationContext(state, navigation, vm_.navigationToken);
            lua_setfield(state, -2, "navigation");
            Detail::PushLuaPhysicsContext(state, physics);
            lua_setfield(state, -2, "physics");
            Detail::PushLuaRuntimeUi(state, vm_.ui);
            lua_setfield(state, -2, "ui");
            Detail::PushLuaCharacterContext(state, character_, characterError_);
            lua_setfield(state, -2, "character");
            lua_newtable(state);
            Function<Position>(state, context, "position");
            Function<SetPosition>(state, context, "set_position");
            lua_setfield(state, -2, "transform");
            lua_newtable(state);
            Function<Action>(state, context, "action");
            lua_setfield(state, -2, "input");
            lua_newtable(state);
            Function<Publish>(state, context, "publish");
            lua_setfield(state, -2, "events");
            lua_newtable(state);
            Function<Field>(state, context, "get");
            lua_setfield(state, -2, "fields");
            lua_newtable(state);
            Function<LogInfo>(state, context, "info");
            lua_setfield(state, -2, "log");
            const GameplayEntityRef entity = context.Entity();
            lua_newtable(state);
            lua_pushinteger(state, entity.index);
            lua_setfield(state, -2, "index");
            lua_pushinteger(state, entity.generation);
            lua_setfield(state, -2, "generation");
            lua_setfield(state, -2, "entity");
        }

        void Close() noexcept {
            if (character_)
                character_->Revoke();
            character_.reset();
            characterError_.reset();
            if (vm_.navigationLifetime)
                vm_.navigationLifetime->RequestCancellation();
            if (vm_.ui)
                vm_.ui->Revoke();
            if (vm_.state != nullptr)
                lua_close(vm_.state);
            vm_ = {};
        }

        [[nodiscard]] bool EnsureLoaded() {
            if (failed_ && failedRevision_ == program_->Revision())
                return false;
            if (failed_) {
                failed_ = false;
                Close();
            }
            if (vm_.state != nullptr && vm_.revision == program_->Revision())
                return true;
            Close();
            return LoadCurrentProgram();
        }

        /** @brief Recreates the instance VM only after its source generation changes. */
        [[nodiscard]] bool LoadCurrentProgram() {
            const auto &impl = *program_->impl_;
            vm_.navigationLifetime.emplace();
            vm_.navigationToken = vm_.navigationLifetime->Token();
            vm_.budget = {0, impl.limits.maximumMemoryBytes};
            vm_.state = lua_newstate(BudgetAllocate, &vm_.budget);
            if (vm_.state == nullptr)
                return Fail("Unable to create Lua behavior VM.");
            Detail::OpenLuaBehaviorSandbox(vm_.state);
            lua_sethook(vm_.state, InstructionLimitHook, LUA_MASKCOUNT, static_cast<int>(impl.limits.maximumInstructionsPerCallback));
            if (luaL_loadbufferx(vm_.state, impl.source.data(), impl.source.size(), impl.sourceName.c_str(), "t") != LUA_OK ||
                lua_pcall(vm_.state, 0, 1, 0) != LUA_OK || !lua_istable(vm_.state, -1))
                return Fail(lua_isstring(vm_.state, -1) ? lua_tostring(vm_.state, -1) : "Lua behavior load failed.");
            vm_.behaviorRef = luaL_ref(vm_.state, LUA_REGISTRYINDEX);
            vm_.revision = program_->Revision();
            return true;
        }

        bool Fail(const std::string &message) {
            if (vm_.ui)
                vm_.ui->Revoke();
            if (character_)
                character_->Revoke();
            LOG_ERROR("gameplay.lua", "%s", message.c_str());
            failed_ = true;
            failedRevision_ = program_->Revision();
            return false;
        }

        static void PushAction(lua_State *state, const GameplayInputAction &action) {
            lua_newtable(state);
            lua_pushlstring(state, action.action.Value().data(), action.action.Value().size());
            lua_setfield(state, -2, "id");
            lua_pushnumber(state, action.x);
            lua_setfield(state, -2, "x");
            lua_pushnumber(state, action.y);
            lua_setfield(state, -2, "y");
            lua_pushboolean(state, action.down);
            lua_setfield(state, -2, "down");
            lua_pushboolean(state, action.pressed);
            lua_setfield(state, -2, "pressed");
            lua_pushboolean(state, action.released);
            lua_setfield(state, -2, "released");
        }

        static void PushEvent(lua_State *state, const GameplayEvent &event) {
            lua_newtable(state);
            lua_pushlstring(state, event.type.Value().data(), event.type.Value().size());
            lua_setfield(state, -2, "type_id");
            lua_pushinteger(state, event.schemaVersion);
            lua_setfield(state, -2, "schema_version");
            lua_newtable(state);
            for (const BehaviorField &field : event.fields) {
                PushFieldValue(state, field.value);
                lua_setfield(state, -2, field.name.c_str());
            }
            lua_setfield(state, -2, "fields");
        }

        void Call(const char *name, BehaviorContext &context, const std::optional<double> delta = std::nullopt,
                  const GameplayInputAction *action = nullptr, const GameplayEvent *event = nullptr) {
            if (!EnsureLoaded())
                return;
            if (!vm_.uiBound) {
                auto ui = program_->impl_->uiScopes.Bind(context.RuntimeUiContext());
                if (ui.HasError()) {
                    Fail(ui.ErrorValue().message);
                    return;
                }
                vm_.ui = std::move(ui).Value();
                vm_.uiBound = true;
            }
            lua_State *state = vm_.state;
            // Every shared owner lives outside the protected inner context constructor and callback.
            const auto physics = context.PhysicsContext();
            const auto navigation = context.NavigationContext();
            try {
                BindCharacter(context);
                Detail::UiScript::Protected(state, [this, &context, &physics, &navigation, name, delta, action, event](lua_State *inner) {
                    lua_rawgeti(inner, LUA_REGISTRYINDEX, vm_.behaviorRef);
                    lua_getfield(inner, -1, name);
                    if (lua_isnil(inner, -1))
                        return 0;
                    PushContext(inner, context, physics, navigation);
                    int argumentCount = 1;
                    if (delta.has_value()) {
                        lua_pushnumber(inner, *delta);
                        ++argumentCount;
                    } else if (action != nullptr) {
                        PushAction(inner, *action);
                        ++argumentCount;
                    } else if (event != nullptr) {
                        PushEvent(inner, *event);
                        ++argumentCount;
                    }
                    lua_call(inner, argumentCount, 0);
                    return 0;
                });
            } catch (const Detail::UiScript::LuaTransportFailure &) {
                const std::string message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "Lua callback failed.";
                lua_pop(state, 1);
                Fail(message);
            } catch (const std::bad_alloc &) {
                Fail("Lua callback context exceeded its memory capacity.");
            }
        }

        LuaBehaviorProgram *program_{};
        Vm vm_{};
        std::shared_ptr<const GameplayCharacterContext> character_;
        std::optional<Error> characterError_;
        bool failed_{};
        std::uint64_t failedRevision_{};
    };

    LuaBehaviorProgram::LuaBehaviorProgram(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    LuaBehaviorProgram::~LuaBehaviorProgram() {
        impl_->uiScopes.Revoke();
    }

    /** @copydoc LuaBehaviorProgram::Compile(std::string,const BehaviorTypeId&,std::string,LuaBehaviorLimits) */
    Result<std::unique_ptr<LuaBehaviorProgram>> LuaBehaviorProgram::Compile(std::string source, const BehaviorTypeId &canonicalTypeId,
                                                                            std::string sourceName, const LuaBehaviorLimits limits) {
        return Compile(std::move(source), canonicalTypeId, std::move(sourceName), limits, {}, {});
    }

    /** @copydoc LuaBehaviorProgram::Compile(std::string,const
     * BehaviorTypeId&,std::string,LuaBehaviorLimits,Network::ReplicationSchemaId,const ModuleId&) */
    Result<std::unique_ptr<LuaBehaviorProgram>> LuaBehaviorProgram::Compile(std::string source, const BehaviorTypeId &canonicalTypeId,
                                                                            std::string sourceName, const LuaBehaviorLimits limits,
                                                                            const Network::ReplicationSchemaId canonicalSchemaId,
                                                                            const ModuleId &canonicalModuleId) {
        if (source.empty() || source.size() > 2U * 1024U * 1024U || !canonicalTypeId.IsValid() || limits.maximumMemoryBytes < 64U * 1024U ||
            limits.maximumInstructionsPerCallback == 0 ||
            limits.maximumInstructionsPerCallback > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(
                MakeError(GameplayErrors::InvalidBehaviorComponent, sourceName + ": Invalid source, identity, or Lua compiler budget."));
        auto parsed = ParseProgram(source, canonicalTypeId, sourceName, limits, canonicalSchemaId, canonicalModuleId);
        if (parsed.HasError()) {
            auto error = parsed.ErrorValue();
            error.message = sourceName + ": " + error.message;
            return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(std::move(error));
        }
        auto impl = std::make_unique<Impl>();
        ParsedProgram data = std::move(parsed).Value();
        impl->descriptor = std::move(data.descriptor);
        impl->replication = std::move(data.replication);
        if (impl->replication) {
            auto registry = Detail::BuildLuaReplicationRegistry(*impl->replication, impl->descriptor);
            if (registry.HasError()) {
                auto error = registry.ErrorValue();
                error.message = sourceName + ": " + error.message;
                return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(std::move(error));
            }
            impl->replicationRegistry = std::move(registry).Value();
        }
        impl->source = std::move(source);
        impl->sourceName = std::move(sourceName);
        impl->limits = limits;
        return Result<std::unique_ptr<LuaBehaviorProgram>>::Success(
            std::unique_ptr<LuaBehaviorProgram>{new LuaBehaviorProgram{std::move(impl)}});  // NOSONAR(cpp:S5950)
    }

    /** @copydoc LuaBehaviorProgram::Descriptor */
    const BehaviorDescriptor &LuaBehaviorProgram::Descriptor() const noexcept {
        return impl_->descriptor;
    }

    /** @copydoc LuaBehaviorProgram::Registration */
    BehaviorRegistration LuaBehaviorProgram::Registration() noexcept {
        return {impl_->descriptor, {this, &LuaBehaviorProgram::CreateInstance, &LuaBehaviorProgram::DestroyInstance}};
    }

    /** @copydoc LuaBehaviorProgram::Clone */
    Result<std::unique_ptr<LuaBehaviorProgram>> LuaBehaviorProgram::Clone() const {
        auto cloned = Compile(impl_->source, impl_->descriptor.typeId, impl_->sourceName, impl_->limits,
                              impl_->replication ? impl_->replication->schema.id : Network::ReplicationSchemaId{},
                              impl_->replication ? impl_->replication->schema.owner : ModuleId{});
        if (cloned.HasError())
            return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(cloned.ErrorValue());
        std::unique_ptr<LuaBehaviorProgram> program = std::move(cloned).Value();
        program->impl_->revision = impl_->revision;
        return Result<std::unique_ptr<LuaBehaviorProgram>>::Success(std::move(program));
    }

    /** @copydoc LuaBehaviorProgram::ReplaceCompatible */
    Result<void> LuaBehaviorProgram::ReplaceCompatible(std::unique_ptr<LuaBehaviorProgram> candidate) {
        const Detail::LuaBehaviorReloadMetadata active{impl_->descriptor, impl_->replication, impl_->replicationRegistry.get(),
                                                       impl_->sourceName};
        std::optional<Detail::LuaBehaviorReloadMetadata> incoming;
        if (candidate)
            incoming.emplace(candidate->impl_->descriptor, candidate->impl_->replication, candidate->impl_->replicationRegistry.get(),
                             candidate->impl_->sourceName);
        if (auto valid = Detail::ValidateLuaBehaviorReplacement(active, incoming ? &*incoming : nullptr); valid.HasError())
            return valid;
        if (impl_->revision == std::numeric_limits<std::uint64_t>::max())
            return LuaFailure(impl_->sourceName + ": Lua source generations exhausted.");
        impl_->characterScopes.Revoke();
        impl_->uiScopes.Revoke();
        impl_->replication = std::move(candidate->impl_->replication);
        impl_->replicationRegistry = std::move(candidate->impl_->replicationRegistry);
        impl_->descriptor.displayName = std::move(candidate->impl_->descriptor.displayName);
        impl_->descriptor.category = std::move(candidate->impl_->descriptor.category);
        impl_->source = std::move(candidate->impl_->source);
        impl_->sourceName = std::move(candidate->impl_->sourceName);
        impl_->limits = candidate->impl_->limits;
        ++impl_->revision;
        return Result<void>::Success();
    }

    /** @copydoc LuaBehaviorProgram::ReplicationDeclaration */
    const GameplayReplicationRegistration *LuaBehaviorProgram::ReplicationDeclaration() const noexcept {
        return impl_->replication ? &*impl_->replication : nullptr;
    }

    /** @copydoc LuaBehaviorProgram::AcquireReplication */
    Result<GameplayReplicationLease> LuaBehaviorProgram::AcquireReplication() const {
        return impl_->replicationRegistry
                   ? impl_->replicationRegistry->Acquire()
                   : Result<GameplayReplicationLease>::Failure(MakeError(GameplayErrors::InvalidReplicationRegistration));
    }

    /** @copydoc LuaBehaviorProgram::ReloadFiles */
    Result<void> LuaBehaviorProgram::ReloadFiles(const std::filesystem::path &sourcePath, const std::filesystem::path &sidecarPath) {
        auto candidate = LoadFiles(sourcePath, sidecarPath, impl_->limits);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        return ReplaceCompatible(std::move(candidate).Value());
    }

    /** @copydoc LuaBehaviorProgram::Revision */
    std::uint64_t LuaBehaviorProgram::Revision() const noexcept {
        return impl_->revision;
    }

    IBehaviorInstance *LuaBehaviorProgram::CreateInstance(void *userData) {  // NOSONAR(cpp:S5008) Binding contract mandates void*.
        return std::make_unique<LuaBehaviorInstance>(*static_cast<LuaBehaviorProgram *>(userData)).release();
    }

    void LuaBehaviorProgram::DestroyInstance(void *, IBehaviorInstance *instance) noexcept {  // NOSONAR(cpp:S5008) Same binding contract.
        const std::unique_ptr<IBehaviorInstance> owned{instance};
    }
}  // namespace Horo::Gameplay
