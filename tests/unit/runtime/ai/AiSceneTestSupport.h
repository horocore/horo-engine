#pragma once

/** @file AiSceneTestSupport.h
 * @brief Shared real Scene/controller fixtures for AI activation and canonical restore regressions.
 */
#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AISceneActivation.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::AI::TestSupport {
    [[nodiscard]] inline SourceLocation At(const std::uint32_t line) {
        return SourceLocation{"assets/ai/scene_lifecycle.horo", line, 1};
    }

    [[nodiscard]] inline std::shared_ptr<const BlackboardSchema> MakeSchema(const std::uint64_t identity, const std::uint32_t version = 1,
                                                                            const BlackboardValueKind kind = BlackboardValueKind::Boolean,
                                                                            const BlackboardValue defaultValue = BlackboardScalarValue{
                                                                                false}) {
        const BlackboardKeyDescriptor key{.key = MakeIdentity<BlackboardKeyId>(identity + 1),
                                          .kind = kind,
                                          .cardinality = BlackboardValueCardinality::Scalar,
                                          .maximumCollectionElements = 1,
                                          .presence = BlackboardKeyPresence::Required,
                                          .access = BlackboardKeyAccess::ReadWrite,
                                          .defaultValue = defaultValue};
        auto captured = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(identity),
                                                   .version = version,
                                                   .unknownValuePolicy = BlackboardUnknownValuePolicy::Reject,
                                                   .keys = std::array{key}});
        REQUIRE(captured.HasValue());
        return std::make_shared<const BlackboardSchema>(std::move(captured).Value());
    }

    [[nodiscard]] inline std::shared_ptr<const DecisionAssetPlan> MakePlan(const std::shared_ptr<const BlackboardSchema> &schema,
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

    [[nodiscard]] inline ActivationFixture MakeFixture(const std::uint64_t agentIdentity, const std::uint64_t controllerIdentity,
                                                       const std::uint64_t sceneIdentity, const std::uint32_t entityIndex,
                                                       const std::uint32_t schemaVersion = 1) {
        auto schema = MakeSchema(sceneIdentity * 10, schemaVersion);
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

    [[nodiscard]] inline std::unique_ptr<AiSceneActivationCandidate> Publish(AiSceneRuntime &runtime, ActivationFixture &fixture) {
        auto prepared = runtime.PrepareScene(fixture.binding, std::array{fixture.agent}, std::array{fixture.descriptor});
        REQUIRE(prepared.HasValue());
        auto candidate = std::move(prepared).Value();
        REQUIRE(candidate->ValidatePublication().HasValue());
        candidate->Publish();
        return candidate;
    }

    /** @brief Exercises restore through a real RuntimeScene and the existing AI publication authority. */
    struct RestoreHarness final {
        AiSceneRuntime runtime{std::move(AiSceneRuntime::Create()).Value()};
        std::array<ActivationFixture, 2> fixtures{MakeFixture(1, 11, 7, 0, 2), MakeFixture(2, 11, 7, 1, 2)};
        std::unique_ptr<Runtime::RuntimeScene> scene;
        std::unique_ptr<AiSceneActivationCandidate> publication;
        AgentHandle handle;

        explicit RestoreHarness(const bool entityValues = false) {
            if (entityValues) {
                const BlackboardValue entity = BlackboardScalarValue{BlackboardStoredEntityReference{7, 1, 1}};
                for (auto &fixture : fixtures) {
                    fixture.schema = MakeSchema(70, 2, BlackboardValueKind::EntityReference, entity);
                    fixture.plan = MakePlan(fixture.schema, 71);
                    fixture.descriptor.blackboardSchema = fixture.schema;
                    fixture.descriptor.decisionPlan = fixture.plan;
                }
            }
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{7}, Runtime::SceneDefinitionRevision{1}};
            for (std::size_t index = 0; index < fixtures.size(); ++index)
                builder.Add(Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{index + 1},
                                                             .parent = std::nullopt,
                                                             .localTransform = {},
                                                             .primitiveMesh = std::nullopt,
                                                             .components = {.aiAgent = fixtures[index].agent.agent,
                                                                            .aiController = fixtures[index].agent.controller}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto created = Runtime::RuntimeScene::Create(definition.Value(), fixtures.front().binding.scene);
            REQUIRE(created.HasValue());
            scene = std::move(created).Value();
            const auto agents = std::array{fixtures[0].agent, fixtures[1].agent};
            auto prepared = runtime.PrepareScene(fixtures.front().binding, agents, std::array{fixtures.front().descriptor});
            REQUIRE(prepared.HasValue());
            publication = std::move(prepared).Value();
            publication->Publish();
            handle = runtime.Snapshot().Value().Agents().front().handle;
        }

        [[nodiscard]] AiCanonicalState Capture() const {
            auto state = runtime.CaptureCanonicalState(scene->View());
            REQUIRE(state.HasValue());
            return std::move(state).Value();
        }

        [[nodiscard]] std::unique_ptr<AiSceneRestoreCandidate> Stage(const AiCanonicalState &state,
                                                                     const std::span<const BlackboardSchemaMigration> migrations = {},
                                                                     const CancellationToken cancellation = {}) {
            auto candidate = runtime.PrepareRestoreAtSafePoint(scene->View(), fixtures.front().binding, state, migrations, cancellation);
            REQUIRE(candidate.HasValue());
            return std::move(candidate).Value();
        }

        void StartTask() {
            REQUIRE(runtime.StartTaskAtSafePoint(handle, MakeIdentity<TaskId>(41)).HasValue());
        }

        [[nodiscard]] static BlackboardCanonicalState &Blackboard(AiCanonicalState &state, const std::size_t index = 0) {
            return *state.agents[index].controller->blackboard;
        }
    };

}  // namespace Horo::AI::TestSupport
