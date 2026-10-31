#include "Horo/Gameplay/LuaDecisionNode.h"

#include "LuaBehaviorSandbox.h"
#include "LuaDecisionNodeBoundary.h"

#include <charconv>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Gameplay {
    namespace {
        using namespace AI;
        using Detail::DecisionScript::Protected;

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        }

        void BudgetHook(lua_State *state, lua_Debug *) {
            luaL_error(state, "Decision node instruction budget exceeded");
        }

        void RawField(lua_State *state, int table, const char *name) {
            table = lua_absindex(state, table);
            const bool read = Protected(state, [table, name](lua_State *inner) {
                lua_pushstring(inner, name);
                lua_rawget(inner, table);
                return 1;
            }, true);
            if (!read)
                throw std::invalid_argument("Unable to read node metadata");
        }

        void RawIndex(lua_State *state, lua_Integer index) {
            const int table = lua_absindex(state, -1);
            const bool read = Protected(state, [table, index](lua_State *inner) {
                lua_rawgeti(inner, table, index);
                return 1;
            }, true);
            if (!read)
                throw std::invalid_argument("Unable to read node metadata list");
        }

        std::uint64_t Identity(lua_State *state, int table, const char *name) {
            RawField(state, table, name);
            std::size_t size{};
            const char *text = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &size) : nullptr;
            std::uint64_t value{};
            if (text) {
                const auto parsed = std::from_chars(text, text + size, value);
                if (parsed.ec != std::errc{} || parsed.ptr != text + size)
                    value = 0;
            }
            lua_pop(state, 1);
            return value;
        }

        lua_Integer Integer(lua_State *state, int table, const char *name, lua_Integer fallback = 0) {
            RawField(state, table, name);
            if (!lua_isnil(state, -1) && !lua_isinteger(state, -1))
                throw std::invalid_argument("Expected integer metadata");
            const auto result = lua_isnil(state, -1) ? fallback : lua_tointeger(state, -1);
            lua_pop(state, 1);
            return result;
        }

        bool Boolean(lua_State *state, int table, const char *name) {
            RawField(state, table, name);
            if (!lua_isnil(state, -1) && !lua_isboolean(state, -1))
                throw std::invalid_argument("Expected boolean metadata");
            const bool result = lua_toboolean(state, -1);
            lua_pop(state, 1);
            return result;
        }

        template <typename T> T Enumeration(lua_State *state, int table, const char *name, T count, lua_Integer fallback = 0) {
            const auto value = Integer(state, table, name, fallback);
            if (value < 0 || value >= static_cast<lua_Integer>(count))
                throw std::invalid_argument("Unknown node enum value");
            return static_cast<T>(value);
        }

        void ExactKeys(lua_State *state, int table, std::span<const char *const> names) {
            table = lua_absindex(state, table);
            const bool valid = Protected(state, [table, names](lua_State *inner) {
                lua_pushnil(inner);
                while (lua_next(inner, table) != 0) {
                    if (lua_type(inner, -2) != LUA_TSTRING)
                        throw std::invalid_argument("Expected metadata field name");
                    std::size_t size{};
                    const char *name = lua_tolstring(inner, -2, &size);
                    bool known{};
                    for (const auto *candidate : names)
                        known = known || std::string_view(name, size) == candidate;
                    if (!known)
                        throw std::invalid_argument("Unknown decision node metadata field");
                    lua_pop(inner, 1);
                }
                return 0;
            }, true);
            if (!valid)
                throw std::invalid_argument("Unable to inspect node metadata shape");
        }

        void ListShape(lua_State *state, std::size_t count) {
            const int table = lua_absindex(state, -1);
            const bool valid = Protected(state, [table, count](lua_State *inner) {
                lua_pushnil(inner);
                while (lua_next(inner, table) != 0) {
                    if (!lua_isinteger(inner, -2))
                        throw std::invalid_argument("Expected contiguous metadata list");
                    const auto index = lua_tointeger(inner, -2);
                    if (index < 1 || static_cast<std::size_t>(index) > count)
                        throw std::invalid_argument("Invalid metadata list index");
                    lua_pop(inner, 1);
                }
                return 0;
            }, true);
            if (!valid)
                throw std::invalid_argument("Unable to inspect metadata list");
        }

        std::string Text(lua_State *state, int table, const char *name) {
            RawField(state, table, name);
            std::size_t size{};
            const char *text = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &size) : nullptr;
            if (!text || size > BehaviorTreeAssetHardLimits::PropertyBytes)
                throw std::invalid_argument("Invalid node text");
            std::string result(text, size);
            lua_pop(state, 1);
            return result;
        }

        template <typename T> T RequiredIdentity(lua_State *state, int table, const char *name) {
            auto result = T::Create(Identity(state, table, name));
            if (result.HasError())
                throw std::invalid_argument("Invalid node identity");
            return result.Value();
        }

        std::string ReadStringProperty(lua_State *state) {
            if (lua_type(state, -1) != LUA_TSTRING)
                throw std::invalid_argument("Invalid string property");
            std::size_t size{};
            const char *text = lua_tolstring(state, -1, &size);
            if (size > BehaviorTreeAssetHardLimits::PropertyBytes)
                throw std::invalid_argument("String property exceeds bound");
            return std::string(text, size);
        }

        /** @brief Metadata decoding uses protected raw reads; native allocations unwind through the protected frame. */
        BehaviorTreeProperty ReadProperty(lua_State *state) {
            constexpr const char *keys[]{"id", "kind", "value"};
            ExactKeys(state, -1, keys);
            BehaviorTreeProperty property;
            property.id = RequiredIdentity<BehaviorTreePropertyId>(state, -1, "id");
            property.kind = Enumeration(state, -1, "kind", BehaviorTreePropertyKind::Count, -1);
            RawField(state, -1, "value");
            switch (property.kind) {
                case BehaviorTreePropertyKind::Boolean:
                    if (!lua_isboolean(state, -1))
                        throw std::invalid_argument("Invalid boolean property");
                    property.value = static_cast<bool>(lua_toboolean(state, -1));
                    break;
                case BehaviorTreePropertyKind::SignedInteger:
                    if (!lua_isinteger(state, -1))
                        throw std::invalid_argument("Invalid integer property");
                    property.value = static_cast<std::int64_t>(lua_tointeger(state, -1));
                    break;
                case BehaviorTreePropertyKind::Scalar:
                    if (lua_type(state, -1) != LUA_TNUMBER)
                        throw std::invalid_argument("Invalid scalar property");
                    property.value = static_cast<double>(lua_tonumber(state, -1));
                    break;
                case BehaviorTreePropertyKind::String:
                    property.value = ReadStringProperty(state);
                    break;
                case BehaviorTreePropertyKind::NodeReference:
                    property.value = RequiredIdentity<BehaviorTreeNodeId>(state, -2, "value");
                    break;
                default:
                    throw std::invalid_argument("Unsupported property kind");
            }
            lua_pop(state, 1);
            return property;
        }

        DecisionNodePort ReadPort(lua_State *state) {
            constexpr const char *keys[]{"id", "direction", "kind", "cardinality", "required"};
            ExactKeys(state, -1, keys);
            return {.id = Identity(state, -1, "id"),
                    .direction = Enumeration(state, -1, "direction", DecisionPortDirection::Count),
                    .kind = Enumeration(state, -1, "kind", BlackboardValueKind::Count),
                    .cardinality = Enumeration(state, -1, "cardinality", BlackboardValueCardinality::Count),
                    .required = Boolean(state, -1, "required")};
        }

        DecisionBlackboardRequirement ReadRequirement(lua_State *state) {
            constexpr const char *keys[]{"key", "kind", "cardinality", "access", "presence", "requireDefault"};
            ExactKeys(state, -1, keys);
            return {.key = RequiredIdentity<BlackboardKeyId>(state, -1, "key"),
                    .kind = Enumeration(state, -1, "kind", BlackboardValueKind::Count),
                    .cardinality = Enumeration(state, -1, "cardinality", BlackboardValueCardinality::Count),
                    .access = Enumeration(state, -1, "access", BlackboardKeyAccess::Count),
                    .presence = Enumeration(state, -1, "presence", BlackboardKeyPresence::Count, 1),
                    .requireDefault = Boolean(state, -1, "requireDefault"),
                    .source = {}};
        }

        template <typename T, typename Reader>
        void ReadList(lua_State *state, int table, const char *name, std::size_t maximum, std::vector<T> &output, Reader reader) {
            RawField(state, table, name);
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                return;
            }
            if (!lua_istable(state, -1))
                throw std::invalid_argument("Expected metadata list");
            const auto count = lua_rawlen(state, -1);
            if (count > maximum)
                throw std::invalid_argument("Metadata list exceeds bound");
            ListShape(state, count);
            for (std::size_t index = 1; index <= count; ++index) {
                RawIndex(state, static_cast<lua_Integer>(index));
                if (!lua_istable(state, -1))
                    throw std::invalid_argument("Expected metadata record");
                output.push_back(reader(state));
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }

        void ValidateCallback(lua_State *state, int table, const char *name, bool required) {
            RawField(state, table, name);
            const bool valid = lua_isfunction(state, -1) || (!required && lua_isnil(state, -1));
            lua_pop(state, 1);
            if (!valid)
                throw std::invalid_argument("Missing or malformed node callback");
        }

        void ReadContract(lua_State *state, int table, DecisionNodeContract &contract) {
            constexpr const char *keys[]{"role",   "capabilities", "ports",   "properties", "requirements", "start",
                                         "resume", "abort",        "cleanup", "check",      "service"};
            ExactKeys(state, table, keys);
            const auto role = Text(state, table, "role");
            if (role == "task")
                contract.role = DecisionNodeRole::Task;
            else if (role == "condition")
                contract.role = DecisionNodeRole::Condition;
            else if (role == "service")
                contract.role = DecisionNodeRole::Service;
            else
                throw std::invalid_argument("Unknown decision node role");
            const auto capabilities = Integer(state, table, "capabilities");
            if (capabilities < 0 || capabilities > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("Invalid node capabilities");
            contract.capabilities.bits = static_cast<std::uint32_t>(capabilities);
            ReadList(state, table, "ports", 128, contract.ports, ReadPort);
            ReadList(state, table, "properties", 64, contract.properties, ReadProperty);
            ReadList(state, table, "requirements", 128, contract.descriptor.requirements, ReadRequirement);
        }

        void ValidateCallbacks(lua_State *state, int table, DecisionNodeRole role) {
            const bool task = role == DecisionNodeRole::Task;
            ValidateCallback(state, table, "start", task);
            ValidateCallback(state, table, "resume", task);
            ValidateCallback(state, table, "abort", task);
            ValidateCallback(state, table, "cleanup", task);
            ValidateCallback(state, table, "check", role == DecisionNodeRole::Condition);
            ValidateCallback(state, table, "service", role == DecisionNodeRole::Service);
        }

        struct NodeVm final {
            Detail::LuaBudget budget;
            lua_State *state{};
            int table{LUA_NOREF};
            LuaBehaviorLimits limits;

            explicit NodeVm(LuaBehaviorLimits policy) : budget{0, policy.maximumMemoryBytes}, limits(policy) {
                state = lua_newstate(Detail::BudgetAllocate, &budget);
            }

            ~NodeVm() {
                if (state)
                    lua_close(state);
            }

            bool Load(const std::string &source, const SourceLocation &location) {
                if (!state)
                    return false;
                return Protected(state, [this, &source, &location](lua_State *inner) {
                    Detail::OpenLuaBehaviorSandbox(inner);
                    lua_sethook(inner, BudgetHook, LUA_MASKCOUNT, static_cast<int>(limits.maximumInstructionsPerCallback));
                    if (luaL_loadbufferx(inner, source.data(), source.size(), location.source.c_str(), "t") != LUA_OK)
                        return lua_error(inner);
                    lua_call(inner, 0, 1);
                    if (!lua_istable(inner, -1))
                        return luaL_error(inner, "Expected decision node table");
                    table = luaL_ref(inner, LUA_REGISTRYINDEX);
                    return 0;
                });
            }
        };

        void PushUnsigned(lua_State *state, std::uint64_t value) {
            char text[21];
            const auto result = std::to_chars(text, text + sizeof(text), value);
            lua_pushlstring(state, text, static_cast<std::size_t>(result.ptr - text));
        }

        void PushProperty(lua_State *state, const BehaviorTreeProperty &property) {
            if (const auto *boolean = std::get_if<bool>(&property.value))
                lua_pushboolean(state, *boolean);
            else if (const auto *integer = std::get_if<std::int64_t>(&property.value))
                lua_pushinteger(state, *integer);
            else if (const auto *scalar = std::get_if<double>(&property.value))
                lua_pushnumber(state, *scalar);
            else if (const auto *text = std::get_if<std::string>(&property.value))
                lua_pushlstring(state, text->data(), text->size());
            else if (const auto *node = std::get_if<BehaviorTreeNodeId>(&property.value))
                PushUnsigned(state, node->Value());
            else
                lua_pushnil(state);
        }

        void PushScalar(lua_State *state, const BlackboardScalarValue &value) {
            std::visit([state](const auto &scalar) {
                using T = std::decay_t<decltype(scalar)>;
                if constexpr (std::is_same_v<T, bool>)
                    lua_pushboolean(state, scalar);
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    lua_pushinteger(state, scalar);
                else if constexpr (std::is_same_v<T, double>)
                    lua_pushnumber(state, scalar);
                else if constexpr (std::is_same_v<T, BlackboardStoredEntityReference>) {
                    lua_newtable(state);
                    PushUnsigned(state, scalar.sceneIncarnation);
                    lua_setfield(state, -2, "scene");
                    lua_pushinteger(state, scalar.slot);
                    lua_setfield(state, -2, "slot");
                    lua_pushinteger(state, scalar.generation);
                    lua_setfield(state, -2, "generation");
                } else if constexpr (std::is_same_v<T, BlackboardStoredAssetReference>) {
                    lua_pushlstring(state, reinterpret_cast<const char *>(scalar.canonicalBytes.data()), scalar.canonicalBytes.size());
                } else {
                    lua_newtable(state);
                    const auto millimeters = scalar.Millimeters();
                    for (std::size_t index = 0; index < millimeters.size(); ++index) {
                        lua_pushinteger(state, millimeters[index]);
                        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
                    }
                }
            }, value);
        }

        void PushBlackboardValue(lua_State *state, const std::optional<BlackboardValue> &value) {
            if (!value) {
                lua_pushnil(state);
                return;
            }
            if (const auto *scalar = std::get_if<BlackboardScalarValue>(&*value)) {
                PushScalar(state, *scalar);
                return;
            }
            const auto *collection = std::get_if<BlackboardCollectionValue>(&*value);
            if (!collection)
                throw std::invalid_argument("Opaque blackboard values cannot execute");
            lua_newtable(state);
            for (std::size_t index = 0; index < collection->count; ++index) {
                PushScalar(state, collection->elements[index]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
            }
        }

        struct BoundValue final {
            BlackboardKeyId key;
            std::optional<BlackboardValue> value;
        };

        Result<void> ReadBlackboard(const BehaviorTreeEvaluationContext *context, std::vector<BoundValue> &values) {
            values.clear();
            if (!context)
                return Result<void>::Success();
            if (context->bindings.size() > 2 * DecisionAssetValidationHardLimits::RequirementsPerNode)
                return Invalid<void>();
            for (const auto &binding : context->bindings) {
                auto value = context->blackboard.Read(binding.key);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                values.push_back({binding.key, std::move(value).Value()});
            }
            return Result<void>::Success();
        }

        bool ValidLuaIdentity(const std::string &source, DecisionNodeTypeId type, DecisionProviderId provider, std::uint32_t version) {
            return source.size() <= 256U * 1024U && type.IsValid() && provider.IsValid() && version != 0;
        }

        bool ValidLuaPolicy(LuaBehaviorLimits limits) {
            return limits.maximumMemoryBytes > 0 && limits.maximumMemoryBytes <= 16U * 1024U * 1024U &&
                   limits.maximumInstructionsPerCallback > 0 && limits.maximumInstructionsPerCallback <= 1'000'000;
        }

        bool ValidLuaSource(const SourceLocation &location) {
            return !location.source.empty() && location.source.size() <= 4096 && (location.line != 0 || location.column == 0);
        }

        int CallbackResult(lua_State *state) {
            if (lua_isboolean(state, -1))
                return lua_toboolean(state, -1) ? -3 : -2;
            if (lua_isnil(state, -1))
                return 0;
            if (!lua_isinteger(state, -1))
                throw std::invalid_argument("Expected typed node callback result");
            const auto result = lua_tointeger(state, -1);
            if (result < 0 || result > static_cast<lua_Integer>(AiTaskState::Cancelled))
                throw std::invalid_argument("Invalid node task status");
            return static_cast<int>(result);
        }
    }  // namespace

    struct LuaDecisionNodeProgram::Image final {
        std::string source;
        SourceLocation sourceLocation;
        AI::DecisionNodeContract contract;
        LuaBehaviorLimits limits;
        std::atomic_bool admission{true};
        std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<const AI::DecisionNodeGeneration> provider;
        static AI::IBehaviorTreeExecutor *Create(void *image, std::span<const AI::BehaviorTreeProperty> properties);

        static void Destroy(void *, AI::IBehaviorTreeExecutor *instance) noexcept {
            delete instance;
        }

        bool CanAdmit() const noexcept {
            return std::this_thread::get_id() == owner && admission.load(std::memory_order_acquire) && (!provider || provider->CanAdmit());
        }

        Result<void> ReadDeclarations(AI::DecisionNodeTypeId type, AI::DecisionProviderId origin, std::uint32_t version) {
            contract.descriptor = {.type = type,
                                   .origin = {AI::DecisionDescriptorSourceKind::Script, origin, version},
                                   .requirements = {},
                                   .source = sourceLocation};
            NodeVm compiler(limits);
            if (!compiler.Load(source, sourceLocation))
                return Invalid<void>();
            const bool parsed = Protected(compiler.state, [this, &compiler](lua_State *state) {
                lua_rawgeti(state, LUA_REGISTRYINDEX, compiler.table);
                ReadContract(state, -1, contract);
                ValidateCallbacks(state, -1, contract.role);
                return 0;
            });
            if (!parsed)
                return Invalid<void>();
            for (auto &requirement : contract.descriptor.requirements)
                requirement.source = sourceLocation;
            return AI::ValidateDecisionNodeRegistration({contract, {this, Create, Destroy}});
        }
    };

    class LuaDecisionNodeProgram::Instance final : public AI::IBehaviorTreeExecutor {
    public:
        Instance(const Image &image, std::span<const AI::BehaviorTreeProperty> properties)
            : vm_(image.limits), properties_(properties.begin(), properties.end()) {
            if (!vm_.Load(image.source, image.sourceLocation))
                throw std::invalid_argument("Unable to instantiate decision node VM");
            values_.reserve(2 * DecisionAssetValidationHardLimits::RequirementsPerNode);
        }

        Result<AI::AiTaskState> Start(const AI::BehaviorTreeEvaluationContext &context,
                                      const AI::AiTaskOperationContext &) noexcept override {
            return Task("start", context);
        }

        Result<AI::AiTaskState> Resume(const AI::BehaviorTreeEvaluationContext &context,
                                       const AI::AiTaskOperationContext &) noexcept override {
            return Task("resume", context);
        }

        void Abort(const AI::AiTaskOperationContext &, AI::AiTaskCancellationReason reason) noexcept override {
            (void)Call("abort", nullptr, static_cast<int>(reason));
        }

        void Cleanup(const AI::AiTaskOperationContext &, const AI::AiTaskTerminalResult &result) noexcept override {
            (void)Call("cleanup", nullptr, static_cast<int>(result.state));
        }

        Result<bool> Check(const AI::BehaviorTreeEvaluationContext &context) noexcept override {
            auto result = Call("check", &context);
            if (result.HasError())
                return Result<bool>::Failure(result.ErrorValue());
            if (result.Value() != -2 && result.Value() != -3)
                return Invalid<bool>();
            return Result<bool>::Success(result.Value() == -3);
        }

        Result<void> Service(const AI::BehaviorTreeEvaluationContext &context) noexcept override {
            auto result = Call("service", &context);
            return result.HasError() ? Result<void>::Failure(result.ErrorValue()) : Result<void>::Success();
        }

    private:
        Result<AI::AiTaskState> Task(const char *name, const AI::BehaviorTreeEvaluationContext &context) noexcept {
            auto result = Call(name, &context);
            if (result.HasError())
                return Result<AI::AiTaskState>::Failure(result.ErrorValue());
            if (result.Value() < static_cast<int>(AI::AiTaskState::Running) ||
                result.Value() > static_cast<int>(AI::AiTaskState::Cancelled))
                return Invalid<AI::AiTaskState>();
            return Result<AI::AiTaskState>::Success(static_cast<AI::AiTaskState>(result.Value()));
        }

        void PushContext(lua_State *state, const AI::BehaviorTreeEvaluationContext *context, int status,
                         std::span<const BoundValue> values) const {
            lua_newtable(state);
            if (context) {
                PushUnsigned(state, context->node.Value());
                lua_setfield(state, -2, "node");
                PushUnsigned(state, context->tick);
                lua_setfield(state, -2, "tick");
            }
            lua_pushinteger(state, status);
            lua_setfield(state, -2, "status");
            lua_newtable(state);
            for (const auto &property : properties_) {
                PushUnsigned(state, property.id.Value());
                PushProperty(state, property);
                lua_rawset(state, -3);
            }
            lua_setfield(state, -2, "properties");
            lua_newtable(state);
            for (const auto &value : values) {
                PushUnsigned(state, value.key.Value());
                PushBlackboardValue(state, value.value);
                lua_rawset(state, -3);
            }
            lua_setfield(state, -2, "blackboard");
        }

        Result<int> Call(const char *name, const AI::BehaviorTreeEvaluationContext *context, int status = 0) noexcept {
            try {
                int result{};
                if (auto values = ReadBlackboard(context, values_); values.HasError())
                    return Result<int>::Failure(values.ErrorValue());
                lua_settop(vm_.state, 0);
                const bool called = Protected(vm_.state, [this, name, context, status, &result](lua_State *state) {
                    lua_sethook(state, BudgetHook, LUA_MASKCOUNT, static_cast<int>(vm_.limits.maximumInstructionsPerCallback));
                    lua_rawgeti(state, LUA_REGISTRYINDEX, vm_.table);
                    RawField(state, -1, name);
                    lua_pushvalue(state, -2);
                    PushContext(state, context, status, values_);
                    lua_call(state, 2, 1);
                    result = CallbackResult(state);
                    return 0;
                });
                return called ? Result<int>::Success(result) : Invalid<int>();
            } catch (...) {
                return Invalid<int>();
            }
        }

        NodeVm vm_;
        std::vector<AI::BehaviorTreeProperty> properties_;
        std::vector<BoundValue> values_;
    };

    AI::IBehaviorTreeExecutor *LuaDecisionNodeProgram::Image::Create(void *image, std::span<const AI::BehaviorTreeProperty> properties) {
        return std::make_unique<Instance>(*static_cast<const Image *>(image), properties).release();
    }

    Result<std::unique_ptr<LuaDecisionNodeProgram>> LuaDecisionNodeProgram::Compile(
        std::string source, AI::DecisionNodeTypeId type, AI::DecisionProviderId provider, std::uint32_t version,
        SourceLocation sourceLocation, LuaBehaviorLimits limits, std::shared_ptr<const AI::DecisionNodeGeneration> providerGeneration) {
        using Output = Result<std::unique_ptr<LuaDecisionNodeProgram>>;
        if (!ValidLuaIdentity(source, type, provider, version))
            return Invalid<std::unique_ptr<LuaDecisionNodeProgram>>();
        if (!ValidLuaPolicy(limits))
            return Invalid<std::unique_ptr<LuaDecisionNodeProgram>>();
        if (!ValidLuaSource(sourceLocation))
            return Invalid<std::unique_ptr<LuaDecisionNodeProgram>>();
        if (providerGeneration && !providerGeneration->CanAdmit())
            return Output::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        try {
            auto image = std::make_shared<Image>();
            image->source = std::move(source);
            image->sourceLocation = std::move(sourceLocation);
            image->limits = limits;
            image->provider = std::move(providerGeneration);
            if (auto read = image->ReadDeclarations(type, provider, version); read.HasError())
                return Output::Failure(read.ErrorValue());
            if (!image->CanAdmit())
                return Output::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            return Output::Success(std::unique_ptr<LuaDecisionNodeProgram>{new LuaDecisionNodeProgram{std::move(image)}});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        } catch (...) {
            return Invalid<std::unique_ptr<LuaDecisionNodeProgram>>();
        }
    }

    LuaDecisionNodeProgram::~LuaDecisionNodeProgram() {
        Retire();
    }

    void LuaDecisionNodeProgram::Retire() noexcept {
        image_->admission.store(false, std::memory_order_release);
    }

    Result<std::shared_ptr<const AI::DecisionNodeGeneration>> LuaDecisionNodeProgram::Acquire() const {
        if (!image_->CanAdmit())
            return Result<std::shared_ptr<const AI::DecisionNodeGeneration>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        const AI::DecisionNodeRegistration registration{image_->contract, {image_.get(), Image::Create, Image::Destroy}};
        std::shared_ptr<const std::atomic_bool> admission{image_, &image_->admission};
        return AI::DecisionNodeGeneration::Capture({&registration, 1}, std::move(admission), image_->provider);
    }
}  // namespace Horo::Gameplay
