#include "GameplayAiTestSupport.h"
#include "Horo/Gameplay/GameplayAiScriptBridge.h"

#include <algorithm>
#include <limits>

namespace Horo::Gameplay {
    using namespace Extensions;

    namespace {
        /** @brief Real admitted import and existing invocation registry above the real native AI owner. */
        struct ScriptFixture final {
            AiTestSupport::Fixture native;
            std::shared_ptr<ScriptInvocationRegistry> registry{std::make_shared<ScriptInvocationRegistry>()};
            ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project};
            ScriptCapabilityScope runtime{ScriptCapabilityScopeKind::Runtime};
            ScriptCapabilityScope scene{ScriptCapabilityScopeKind::Scene};
            ScriptCapabilityScope operation{ScriptCapabilityScopeKind::Operation};
            ExtensionCapabilityAdmission admission;
            ScriptInvocationProviderRegistration provider;
            std::optional<ScriptCapabilityContext> context;
            std::optional<ScriptCapabilityBinding> binding;
            GameplayAiScriptBridge bridge{native.context};

            explicit ScriptFixture(const AI::BlackboardValue &value = AI::BlackboardScalarValue{false})
                : native(value), admission(Admission()), provider(Provider()) {
                auto api = MakeGameplayAiScriptDescriptor("com.example.module", {"horo.ai"});
                std::vector<std::string> functions;
                for (const auto &function : api.functions)
                    functions.push_back(function.id);
                auto descriptors = BuildScriptExportDescriptorSnapshot(std::array{api});
                if (descriptors.HasError())
                    INFO(descriptors.ErrorValue().message);
                REQUIRE(descriptors.HasValue());
                auto grant = admission.Grant({"horo.ai"});
                REQUIRE(grant.HasValue());
                ScriptCapabilityContextDescriptor descriptor{.scriptId = "com.example.script",
                                                             .policyRevision = 1,
                                                             .profile = ScriptCapabilityProfile::Gameplay,
                                                             .packageVerified = true,
                                                             .packageTrusted = true,
                                                             .scriptTrusted = true,
                                                             .runtimeCompatible = true,
                                                             .packageCapabilities = {{"horo.ai"}},
                                                             .scriptCapabilities = {{"horo.ai"}},
                                                             .projectCapabilities = {{"horo.ai"}},
                                                             .contextCapabilities = {{"horo.ai"}},
                                                             .scopes = {project.Lease(), runtime.Lease(), scene.Lease(), operation.Lease()},
                                                             .imports = {{std::string{GameplayAiScriptApiId},
                                                                          {{1, 0, 0}, {1, 0, 0}},
                                                                          descriptors.Value(),
                                                                          grant.Value(),
                                                                          provider.Lease(),
                                                                          ScriptCapabilityProfile::Gameplay,
                                                                          std::move(functions)}}};
                auto created = ScriptCapabilityContext::Create(registry, std::move(descriptor));
                REQUIRE(created.HasValue());
                context.emplace(std::move(created).Value());
                auto bound = context->Bind(GameplayAiScriptApiId);
                REQUIRE(bound.HasValue());
                binding.emplace(std::move(bound).Value());
            }

            ExtensionCapabilityAdmission Admission() {
                auto admitted = ExtensionCapabilityAdmission::Evaluate({.extensionId = "com.example.package",
                                                                        .moduleId = "com.example.module",
                                                                        .activationGeneration = 1,
                                                                        .capabilities = {{{"horo.ai"}, {{"ai.use"}}}}},
                                                                       {.revision = 1,
                                                                        .knownPermissions = {{"ai.use"}},
                                                                        .approvedPermissions = {{"ai.use"}},
                                                                        .availableCapabilities = {{"horo.ai"}}});
                REQUIRE(admitted.HasValue());
                return std::move(admitted).Value();
            }

            ScriptInvocationProviderRegistration Provider() {
                auto created = registry->RegisterProvider({.generation = 1});
                REQUIRE(created.HasValue());
                return std::move(created).Value();
            }

            ScriptCallResult Call(std::string function, std::vector<ScriptValue> arguments) {
                auto invoked =
                    bridge.InvokeAtSafePoint(*binding, provider, {.functionId = std::move(function), .arguments = std::move(arguments)});
                REQUIRE(invoked.HasValue());
                auto snapshot = invoked.Value().Snapshot();
                REQUIRE(snapshot);
                REQUIRE(snapshot->terminalResult);
                auto result = *snapshot->terminalResult;
                REQUIRE(context->Drain().HasValue());
                return result;
            }

            ScriptValue Value(std::string function, std::vector<ScriptValue> arguments) {
                auto result = Call(std::move(function), std::move(arguments));
                if (result.error)
                    INFO(result.error->code);
                REQUIRE(result.IsSuccess());
                REQUIRE(result.values.size() == 1);
                return result.values.front();
            }

            ScriptValue Agent() {
                return Value("resolve-agent", {ScriptValue{std::uint64_t{1}}});
            }
        };

        /** @brief Changes only a typed boolean payload while retaining complete ABI-required logical fields. */
        ScriptValue BooleanValue(const ScriptValue &source, const bool boolean) {
            ScriptValue::StructFields value{source.AsStruct().begin(), source.AsStruct().end()};
            for (auto &[name, field] : value)
                if (name == "scalar") {
                    ScriptValue::StructFields scalar{field.AsStruct().begin(), field.AsStruct().end()};
                    for (auto &[id, item] : scalar)
                        if (id == "boolean")
                            item = ScriptValue{boolean};
                    field = ScriptValue::Struct("ai-scalar", std::move(scalar));
                }
            return ScriptValue::Struct("ai-value", std::move(value));
        }
    }  // namespace

    TEST_CASE("Script AI descriptor admits canonical names and rejects underscore declarations", "[gameplay][ai][script]") {
        auto api = MakeGameplayAiScriptDescriptor("com.example.module", {"horo.ai"});
        auto snapshot = BuildScriptExportDescriptorSnapshot(std::array{api});
        if (snapshot.HasError())
            INFO(snapshot.ErrorValue().message);
        REQUIRE(snapshot.HasValue());
        const auto *admitted = snapshot.Value()->Find(GameplayAiScriptApiId);
        REQUIRE(admitted);
        CHECK(admitted->functions.size() == 8);
        CHECK(admitted->errors.size() == 17);
        CHECK(std::ranges::any_of(admitted->errors, [](const auto &error) {
            return error.id == "ai.native-error";
        }));
        CHECK(std::ranges::any_of(admitted->functions, [](const auto &function) {
            return function.id == "resolve-agent";
        }));
        CHECK(std::ranges::any_of(admitted->errors, [](const auto &error) {
            return error.id == "ai.scene.capability-unavailable";
        }));

        SECTION("Types retain strict canonical admission") {
            api.types.front().id = "ai_scalar";
        }
        SECTION("Native error codes require an explicit script identity") {
            api.errors.front().id = AI::AIErrors::CapabilityUnavailable.code.Value();
        }
        CHECK(BuildScriptExportDescriptorSnapshot(std::array{api}).HasError());
    }

    TEST_CASE("Script AI opaque operations mutate the same native scene task lifecycle", "[gameplay][ai][script]") {
        ScriptFixture fixture;
        auto agent = fixture.Agent();
        auto task = fixture.Value("start-task", {agent, ScriptValue{std::uint64_t{42}}});
        REQUIRE(task.AsHandle());
        CHECK(task.AsHandle()->type == "horo.ai.task");
        CHECK(fixture.native.runtime.Find(fixture.native.agent).Value().hasRunningTask);
        auto running = fixture.Value("task-state", {task});
        REQUIRE(running.AsUnsignedInteger());
        CHECK(*running.AsUnsignedInteger() == static_cast<std::uint64_t>(AI::AiTaskState::Running));
        REQUIRE(fixture.Call("complete-task", {task}).IsSuccess());
        CHECK(*fixture.Value("task-state", {task}).AsUnsignedInteger() == static_cast<std::uint64_t>(AI::AiTaskState::Succeeded));
        REQUIRE(fixture.Call("release", {task}).IsSuccess());
        CHECK(fixture.Call("task-state", {task}).IsFailure());
        auto native = fixture.native.context->StartTask(fixture.native.agent, AiTestSupport::MakeIdentity<AI::TaskId>(41));
        REQUIRE(native.HasValue());
        REQUIRE(fixture.native.context->CancelTask(fixture.native.agent, native.Value()).HasValue());
        CHECK(fixture.native.context->InspectTask(fixture.native.agent, native.Value()).Value().state == AI::AiTaskState::Cancelled);
    }

    TEST_CASE("Script AI blackboard writes commit through native atomic schema validation", "[gameplay][ai][script]") {
        ScriptFixture fixture;
        auto agent = fixture.Agent();
        const ScriptValue key{std::uint64_t{71}};
        auto old = fixture.Value("read-blackboard", {agent, key});
        const auto write = [&](std::uint64_t identity, bool value) {
            return ScriptValue::Struct("ai-write", {{"key", ScriptValue{identity}}, {"value", BooleanValue(old, value)}});
        };
        auto committed = fixture.Call("write-blackboard", {agent, ScriptValue::Array({write(71, true)})});
        REQUIRE(committed.IsSuccess());
        auto native = fixture.native.context->ReadBlackboard(fixture.native.agent, AiTestSupport::MakeIdentity<AI::BlackboardKeyId>(71));
        REQUIRE(native.HasValue());
        REQUIRE(native.Value());
        CHECK(std::get<bool>(std::get<AI::BlackboardScalarValue>(*native.Value())));
        auto rejected = fixture.Call("write-blackboard", {agent, ScriptValue::Array({write(71, false), write(999, false)})});
        REQUIRE(rejected.IsFailure());
        CHECK(rejected.error->domain == "horo.ai");
        CHECK(std::get<bool>(std::get<AI::BlackboardScalarValue>(
            *fixture.native.context->ReadBlackboard(fixture.native.agent, AiTestSupport::MakeIdentity<AI::BlackboardKeyId>(71)).Value())));
    }

    TEST_CASE("Script AI reload and provider retirement cancel owned work and reject retained handles", "[gameplay][ai][script]") {
        ScriptFixture fixture;
        auto agent = fixture.Agent();
        auto task = fixture.Value("start-task", {agent, ScriptValue{std::uint64_t{42}}});
        SECTION("script reload") {
            fixture.context->Reset();
        }
        SECTION("provider retirement") {
            fixture.provider.Reset();
        }
        SECTION("scene scope closed") {
            fixture.scene.Revoke();
        }
        fixture.bridge.ReapRevokedAtSafePoint();
        CHECK_FALSE(fixture.native.runtime.Find(fixture.native.agent).Value().hasRunningTask);
        CHECK(fixture.bridge.InvokeAtSafePoint(*fixture.binding, fixture.provider, {.functionId = "task-state", .arguments = {task}})
                  .HasError());
        CHECK(fixture.native.context->StartTask(fixture.native.agent, AiTestSupport::MakeIdentity<AI::TaskId>(41)).HasValue());
    }

    TEST_CASE("Script AI missing runtime and undeclared tasks produce owned typed errors", "[gameplay][ai][script]") {
        ScriptFixture fixture;
        auto agent = fixture.Agent();
        auto unknown = fixture.Call("start-task", {agent, ScriptValue{std::uint64_t{999}}});
        REQUIRE(unknown.IsFailure());
        CHECK(unknown.error->code == "ai.scene.capability-unavailable");
        REQUIRE(unknown.error->details.size() == 1);
        CHECK(unknown.error->details.front().id == "native-code");
        CHECK(unknown.error->details.front().value.AsString() == AI::AIErrors::CapabilityUnavailable.code.Value());
        fixture.native.context->Revoke();
        auto revoked = fixture.Call("resolve-agent", {ScriptValue{std::uint64_t{1}}});
        REQUIRE(revoked.IsFailure());
        CHECK(revoked.error->code == "ai.scene.capability-unavailable");
    }

    TEST_CASE("Script AI typed blackboard projection round trips exact scalar and maximum collection values", "[gameplay][ai][script]") {
        AI::BlackboardStoredAssetReference asset;
        asset.canonicalBytes.front() = 1;
        AI::BlackboardCollectionValue collection;
        collection.elementKind = AI::BlackboardValueKind::SignedInteger;
        collection.count = AI::MaximumBlackboardCollectionElements;
        for (std::size_t i = 0; i < collection.count; ++i)
            collection.elements[i] = std::int64_t{static_cast<std::int64_t>(i)};
        const std::array<AI::BlackboardValue, 7>
            values{AI::BlackboardScalarValue{true},
                   AI::BlackboardScalarValue{std::numeric_limits<std::int64_t>::max()},
                   AI::BlackboardScalarValue{3.5},
                   AI::BlackboardScalarValue{AI::BlackboardStoredEntityReference{7, 0, 1}},
                   AI::BlackboardScalarValue{asset},
                   AI::BlackboardScalarValue{Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::min(), 0,
                                                                                      std::numeric_limits<std::int64_t>::max())},
                   collection};
        for (const auto &original : values) {
            ScriptFixture fixture{original};
            auto agent = fixture.Agent();
            auto copied = fixture.Value("read-blackboard", {agent, ScriptValue{std::uint64_t{71}}});
            auto write = ScriptValue::Struct("ai-write", {{"key", ScriptValue{std::uint64_t{71}}}, {"value", copied}});
            auto committed = fixture.Call("write-blackboard", {agent, ScriptValue::Array({std::move(write)})});
            REQUIRE(committed.IsSuccess());
            auto native =
                fixture.native.context->ReadBlackboard(fixture.native.agent, AiTestSupport::MakeIdentity<AI::BlackboardKeyId>(71));
            REQUIRE(native.HasValue());
            REQUIRE(native.Value());
            CHECK(*native.Value() == original);
        }
    }
}  // namespace Horo::Gameplay
