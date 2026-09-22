#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AISceneActivation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        [[nodiscard]] SourceLocation At(const std::uint32_t line) {
            return SourceLocation{"assets/ai/scene_lifecycle.horo", line, 1};
        }

        [[nodiscard]] std::shared_ptr<const BlackboardSchema> MakeSchema(const std::uint64_t identity) {
            const BlackboardKeyDescriptor key{.key = MakeIdentity<BlackboardKeyId>(identity + 1),
                                              .kind = BlackboardValueKind::Boolean,
                                              .cardinality = BlackboardValueCardinality::Scalar,
                                              .maximumCollectionElements = 1,
                                              .presence = BlackboardKeyPresence::Required,
                                              .access = BlackboardKeyAccess::ReadWrite,
                                              .defaultValue = BlackboardValue{BlackboardScalarValue{false}}};
            auto captured = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(identity),
                                                       .version = 1,
                                                       .unknownValuePolicy = BlackboardUnknownValuePolicy::Reject,
                                                       .keys = std::array{key}});
            REQUIRE(captured.HasValue());
            return std::make_shared<const BlackboardSchema>(std::move(captured).Value());
        }

        [[nodiscard]] std::shared_ptr<const DecisionAssetPlan> MakePlan(const std::shared_ptr<const BlackboardSchema> &schema,
                                                                        const std::uint64_t assetIdentity) {
            const DecisionNodeDescriptor nodeDescriptor{.type = MakeIdentity<DecisionNodeTypeId>(assetIdentity + 1),
                                                        .origin = {DecisionDescriptorSourceKind::Native,
                                                                   MakeIdentity<DecisionProviderId>(assetIdentity + 2), 1},
                                                        .requirements = {},
                                                        .source = At(3)};
            const DecisionAssetDescriptor asset{.asset = MakeIdentity<DecisionGraphAssetId>(assetIdentity),
                                                .kind = DecisionPlanKind::BehaviorTree,
                                                .schemaVersion = CurrentDecisionAssetSchemaVersion,
                                                .blackboardSchema = schema->Identity(),
                                                .requiredBlackboardSchemaVersion = {1, std::numeric_limits<std::uint32_t>::max()},
                                                .nodes = {{.id = MakeIdentity<DecisionNodeId>(assetIdentity + 3),
                                                           .type = nodeDescriptor.type,
                                                           .descriptorVersion = {1, 1},
                                                           .requirements = {},
                                                           .source = At(5)}},
                                                .subtrees = {},
                                                .source = At(1)};
            const auto compiled = DecisionAssetCompiler::Compile(asset, {}, std::array{nodeDescriptor}, std::array{schema});
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().IsValid());
            return compiled.Value().plan;
        }

        struct ActivationFixture final {
            std::shared_ptr<const BlackboardSchema> schema;
            std::shared_ptr<const DecisionAssetPlan> plan;
            AiControllerDescriptor descriptor;
            AiSceneActivationBinding binding;
            Runtime::EntityRef owner;
            AiSceneAgentDescriptor agent;
        };

        [[nodiscard]] ActivationFixture MakeFixture(const std::uint64_t agentIdentity, const std::uint64_t controllerIdentity,
                                                    const std::uint64_t sceneIdentity, const std::uint32_t entityIndex) {
            auto schema = MakeSchema(sceneIdentity * 10);
            auto plan = MakePlan(schema, sceneIdentity * 10 + 1);
            AiControllerDescriptor descriptor{.controller = MakeIdentity<ControllerTypeId>(controllerIdentity),
                                              .decisionAsset = plan->Asset(),
                                              .decisionKind = plan->Kind(),
                                              .blackboardSchema = schema,
                                              .decisionPlan = plan};
            const AiSceneActivationBinding binding{.incarnation = AiRuntimeIncarnation::Create(sceneIdentity).Value(),
                                                   .scene = Runtime::SceneRuntimeId{sceneIdentity}};
            const Runtime::EntityRef owner{.runtime = binding.scene, .entity = Runtime::EntityId{.index = entityIndex, .generation = 1}};
            const AiAgentComponent agent{.agent = MakeIdentity<AgentId>(agentIdentity)};
            const AiControllerComponent controller{.controller = descriptor.controller,
                                                   .decisionAsset = descriptor.decisionAsset,
                                                   .blackboardSchema = schema->Identity(),
                                                   .decisionKind = descriptor.decisionKind,
                                                   .requiredCapabilities = AiCapabilitySet::Of(AiCapability::Behavior),
                                                   .schemaVersion = CurrentAiSceneComponentSchemaVersion,
                                                   .startupPolicy = AiStartupPolicy::OnSceneActivation,
                                                   .enabled = true};
            return {.schema = std::move(schema),
                    .plan = std::move(plan),
                    .descriptor = std::move(descriptor),
                    .binding = binding,
                    .owner = owner,
                    .agent = {.owner = owner, .agent = agent, .controller = controller}};
        }

        [[nodiscard]] std::unique_ptr<AiSceneActivationCandidate> Publish(AiSceneRuntime &runtime, ActivationFixture &fixture) {
            auto prepared = runtime.PrepareScene(fixture.binding, std::array{fixture.agent}, std::array{fixture.descriptor});
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate->ValidatePublication().HasValue());
            candidate->Publish();
            return candidate;
        }

        TEST_CASE("AI activation rejects a failed controller admission without publishing an agent or task",
                  "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto missing = MakeFixture(1, 11, 7, 0);
            const auto failed = runtime.PrepareScene(missing.binding, std::array{missing.agent}, std::array<AiControllerDescriptor, 0>{});
            ExpectError(failed, AIErrors::ControllerDescriptorMissing);
            ExpectError(runtime.Snapshot(), AIErrors::RuntimeUnavailable);
            ExpectError(runtime.ActiveBinding(), AIErrors::RuntimeUnavailable);
        }

        TEST_CASE("AI activation is transactional and leaves the prior publication running on replacement failure",
                  "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto first = MakeFixture(21, 31, 17, 0);
            auto published = Publish(runtime, first);
            REQUIRE(published != nullptr);
            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            const AgentHandle handle = snapshot.Value().Agents().front().handle;
            REQUIRE(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(41)).HasValue());

            auto failed = MakeFixture(22, 32, 17, 1);
            const auto rejected = runtime.PrepareScene(failed.binding, std::array{failed.agent}, std::array{first.descriptor});
            ExpectError(rejected, AIErrors::ControllerDescriptorMissing);

            const auto retained = runtime.Find(handle);
            REQUIRE(retained.HasValue());
            CHECK(retained.Value().state == AiAgentActivationState::Active);
            CHECK(retained.Value().hasBlackboard);
            CHECK(retained.Value().hasRunningTask);
        }

        TEST_CASE("Disabling an AI agent cancels owned task state before revoking capabilities", "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto fixture = MakeFixture(51, 61, 27, 0);
            auto published = Publish(runtime, fixture);
            REQUIRE(published != nullptr);
            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            const AgentHandle handle = snapshot.Value().Agents().front().handle;
            REQUIRE(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(71)).HasValue());

            REQUIRE(runtime.DisableAtSafePoint(handle).HasValue());
            const auto disabled = runtime.Find(handle);
            REQUIRE(disabled.HasValue());
            CHECK(disabled.Value().state == AiAgentActivationState::Disabled);
            CHECK_FALSE(disabled.Value().hasBlackboard);
            CHECK_FALSE(disabled.Value().hasRunningTask);
            CHECK(disabled.Value().stagedCapabilities.bits == 0);
            ExpectError(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(72)), AIErrors::HandleInvalid);
        }

        TEST_CASE("Non-scene AI startup policies retain a disabled generation-fenced agent slot", "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto fixture = MakeFixture(75, 85, 30, 0);
            fixture.agent.agent.startupPolicy = AiStartupPolicy::Manual;
            fixture.agent.controller->startupPolicy = AiStartupPolicy::Manual;

            auto prepared = runtime.PrepareScene(fixture.binding, std::array{fixture.agent}, std::array{fixture.descriptor});
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate->ValidatePublication().HasValue());
            candidate->Publish();

            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            const auto &record = snapshot.Value().Agents().front();
            CHECK(record.handle.IsValid());
            CHECK(record.state == AiAgentActivationState::Disabled);
            CHECK_FALSE(record.hasBlackboard);
            CHECK_FALSE(record.hasRunningTask);
            CHECK(record.stagedCapabilities.bits == 0);
        }

        TEST_CASE("Entity destruction and scene replacement fence AI handles and release old work", "[unit][ai][scene][activation]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            auto first = MakeFixture(81, 91, 37, 0);
            auto firstPublished = Publish(runtime, first);
            REQUIRE(firstPublished != nullptr);
            const auto firstSnapshot = runtime.Snapshot();
            REQUIRE(firstSnapshot.HasValue());
            const AgentHandle stale = firstSnapshot.Value().Agents().front().handle;
            REQUIRE(runtime.StartTaskAtSafePoint(stale, MakeIdentity<TaskId>(101)).HasValue());

            REQUIRE(runtime.RetireOwnerAtSafePoint(first.owner).Value() == 1);
            CHECK(runtime.Snapshot().Value().Agents().empty());
            ExpectError(runtime.Find(stale), AIErrors::HandleInvalid);

            auto replacement = MakeFixture(82, 92, 38, 0);
            auto replacementPublished = Publish(runtime, replacement);
            REQUIRE(replacementPublished != nullptr);
            const auto replacementSnapshot = runtime.Snapshot();
            REQUIRE(replacementSnapshot.HasValue());
            REQUIRE(replacementSnapshot.Value().Agents().size() == 1);
            CHECK(replacementSnapshot.Value().Binding() == replacement.binding);
            CHECK(replacementSnapshot.Value().Agents().front().handle.incarnation != stale.incarnation);
            ExpectError(runtime.Find(stale), AIErrors::HandleInvalid);
        }

        TEST_CASE("AI activation participant projects RuntimeScene entities into the generation-bound lifecycle",
                  "[unit][ai][scene][activation]") {
            auto fixture = MakeFixture(111, 121, 47, 0);
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{47}, Runtime::SceneDefinitionRevision{1}};
            builder.Add(
                Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{900},
                                                 .parent = std::nullopt,
                                                 .localTransform = {},
                                                 .primitiveMesh = std::nullopt,
                                                 .components = {.aiAgent = fixture.agent.agent, .aiController = fixture.agent.controller}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto scene = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{47});
            REQUIRE(scene.HasValue());

            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            const auto descriptors = std::array{fixture.descriptor};
            AiSceneActivationParticipant participant{runtime, descriptors};
            auto prepared = participant.Prepare(definition.Value(), scene.Value()->View());
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate->ValidatePublication().HasValue());
            candidate->Publish();

            const auto snapshot = runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 1);
            CHECK(snapshot.Value().Agents().front().owner.runtime == Runtime::SceneRuntimeId{47});
            CHECK(snapshot.Value().Agents().front().owner.entity.generation == 1);
        }
    }  // namespace
}  // namespace Horo::AI
