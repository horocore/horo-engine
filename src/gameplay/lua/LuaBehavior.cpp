#include "Horo/Gameplay/LuaBehavior.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "LuaBehaviorMetadata.h"
#include "LuaReplicationDeclaration.h"

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
        struct LuaBudget {
            std::size_t used{};
            std::size_t maximum{};
        };

        void *BudgetAllocate(void *userData, void *pointer, const std::size_t oldSize, const std::size_t newSize) {
            auto &budget = *static_cast<LuaBudget *>(userData);
            // For a fresh Lua allocation oldSize is an object-type tag, not an allocation size.
            const std::size_t accountedOldSize = pointer != nullptr ? oldSize : 0;
            if (newSize == 0) {
                budget.used = accountedOldSize > budget.used ? 0 : budget.used - accountedOldSize;
                std::free(pointer);  // NOSONAR(cpp:S5025) Lua's allocator ABI is the C realloc/free contract.
                return nullptr;
            }
            const std::size_t retained = accountedOldSize > budget.used ? 0 : budget.used - accountedOldSize;
            if (newSize > budget.maximum || retained > budget.maximum - newSize)
                return nullptr;
            void *resized = std::realloc(pointer, newSize);  // NOSONAR(cpp:S5025) Preserves Lua realloc semantics and performance.
            if (resized != nullptr)
                budget.used = retained + newSize;
            return resized;
        }

        void InstructionLimitHook(lua_State *state, lua_Debug *) {
            luaL_error(state, "Lua behavior instruction budget exceeded");
        }

        int IdentityBehavior(lua_State *state) {
            luaL_checktype(state, 1, LUA_TTABLE);
            lua_settop(state, 1);
            return 1;
        }

        void OpenSandbox(lua_State *state) {
            luaL_requiref(state, "_G", luaopen_base, 1);
            lua_pop(state, 1);
            luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
            lua_pop(state, 1);
            luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
            lua_pop(state, 1);
            luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
            lua_pop(state, 1);
            luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
            lua_pop(state, 1);
            for (const char *name : {"dofile", "loadfile", "collectgarbage", "io", "os", "package", "debug"}) {
                lua_pushnil(state);
                lua_setglobal(state, name);
            }
            lua_newtable(state);
            lua_pushcfunction(state, IdentityBehavior);
            lua_setfield(state, -2, "behavior");
            lua_setglobal(state, "horo");
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
            OpenSandbox(state);
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

        [[nodiscard]] bool Compatible(const BehaviorDescriptor &active, const BehaviorDescriptor &candidate) {
            if (active.typeId != candidate.typeId || active.schemaVersion != candidate.schemaVersion ||
                active.allowMultiple != candidate.allowMultiple || active.fields.size() != candidate.fields.size() ||
                active.phases.size() != candidate.phases.size())
                return false;
            for (std::size_t index = 0; index < active.fields.size(); ++index) {
                if (active.fields[index].name != candidate.fields[index].name ||
                    active.fields[index].defaultValue.index() != candidate.fields[index].defaultValue.index())
                    return false;
            }
            return true;
        }

        /** @brief Reloads preserve gameplay ownership, schema major version and simulation safe points. */
        bool CompatibleReplicationSchedule(const GameplayReplicationRegistration &active,
                                           const GameplayReplicationRegistration &replacement) {
            return active.owner == replacement.owner && active.schema.version.major == replacement.schema.version.major &&
                   active.schedule.capturePhase == replacement.schedule.capturePhase &&
                   active.schedule.applyPhase == replacement.schedule.applyPhase;
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

        static void PushContext(lua_State *state, BehaviorContext &context) {
            lua_newtable(state);
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
            vm_.budget = {0, impl.limits.maximumMemoryBytes};
            vm_.state = lua_newstate(BudgetAllocate, &vm_.budget);
            if (vm_.state == nullptr)
                return Fail("Unable to create Lua behavior VM.");
            OpenSandbox(vm_.state);
            lua_sethook(vm_.state, InstructionLimitHook, LUA_MASKCOUNT, static_cast<int>(impl.limits.maximumInstructionsPerCallback));
            if (luaL_loadbufferx(vm_.state, impl.source.data(), impl.source.size(), impl.sourceName.c_str(), "t") != LUA_OK ||
                lua_pcall(vm_.state, 0, 1, 0) != LUA_OK || !lua_istable(vm_.state, -1))
                return Fail(lua_isstring(vm_.state, -1) ? lua_tostring(vm_.state, -1) : "Lua behavior load failed.");
            vm_.behaviorRef = luaL_ref(vm_.state, LUA_REGISTRYINDEX);
            vm_.revision = program_->Revision();
            return true;
        }

        bool Fail(const std::string &message) {
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
            lua_State *state = vm_.state;
            lua_rawgeti(state, LUA_REGISTRYINDEX, vm_.behaviorRef);
            lua_getfield(state, -1, name);
            if (lua_isnil(state, -1)) {
                lua_pop(state, 2);
                return;
            }
            PushContext(state, context);
            int argumentCount = 1;
            if (delta.has_value()) {
                lua_pushnumber(state, *delta);
                ++argumentCount;
            } else if (action != nullptr) {
                PushAction(state, *action);
                ++argumentCount;
            } else if (event != nullptr) {
                PushEvent(state, *event);
                ++argumentCount;
            }
            if (lua_pcall(state, argumentCount, 0, 0) != LUA_OK) {
                const std::string message = lua_isstring(state, -1) ? lua_tostring(state, -1) : "Lua callback failed.";
                lua_pop(state, 1);
                Fail(message);
            }
            lua_pop(state, 1);
        }

        LuaBehaviorProgram *program_{};
        Vm vm_{};
        bool failed_{};
        std::uint64_t failedRevision_{};
    };

    LuaBehaviorProgram::LuaBehaviorProgram(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    LuaBehaviorProgram::~LuaBehaviorProgram() = default;

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
        if (!candidate || !Compatible(impl_->descriptor, candidate->impl_->descriptor))
            return LuaFailure((candidate ? candidate->impl_->sourceName : impl_->sourceName) +
                              ": Lua behavior reload requires a schema-compatible candidate or play-session restart.");
        if (impl_->replication.has_value() != candidate->impl_->replication.has_value())
            return LuaFailure(candidate->impl_->sourceName + ": Lua replication reload requires the previous schema declaration.");
        if (impl_->replication) {
            const auto &active = *impl_->replication;
            const auto &replacement = *candidate->impl_->replication;
            if (!CompatibleReplicationSchedule(active, replacement))
                return LuaFailure(candidate->impl_->sourceName + ": Lua replication reload changed owner or safe points.");
            const auto previous = impl_->replicationRegistry->Acquire().Value();
            if (const auto compatible = Network::BuildReplicationDescriptorReplacement(previous.Descriptors(), {&replacement.schema, 1},
                                                                                       GameplayReplicationRegistryLimits{}.descriptors);
                compatible.HasError()) {
                auto error = compatible.ErrorValue();
                error.message = candidate->impl_->sourceName + ": " + error.message;
                return Result<void>::Failure(std::move(error));
            }
            const auto codecs = Detail::ValidateLuaReplicationCodecs(active, replacement, candidate->impl_->sourceName);
            if (codecs.HasError())
                return codecs;
        }
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
