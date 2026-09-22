#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AISceneComponents.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

        [[nodiscard]] AiControllerDescriptor MakeDescriptor(const std::shared_ptr<const BlackboardSchema> &schema,
                                                            const std::shared_ptr<const DecisionAssetPlan> &plan,
                                                            const std::uint64_t controllerIdentity) {
            return {.controller = MakeIdentity<ControllerTypeId>(controllerIdentity),
                    .decisionAsset = plan->Asset(),
                    .decisionKind = plan->Kind(),
                    .blackboardSchema = schema,
                    .decisionPlan = plan};
        }

        TEST_CASE("AI scene components validate typed lifecycle bindings without resolving descriptors", "[unit][ai][scene]") {
            const AiAgentComponent agent{.agent = MakeIdentity<AgentId>(1),
                                         .schemaVersion = CurrentAiSceneComponentSchemaVersion,
                                         .startupPolicy = AiStartupPolicy::OnSceneActivation,
                                         .enabled = true};
            const AiControllerComponent controller{.controller = MakeIdentity<ControllerTypeId>(2),
                                                   .decisionAsset = MakeIdentity<DecisionGraphAssetId>(3),
                                                   .blackboardSchema = MakeIdentity<BlackboardSchemaId>(4),
                                                   .decisionKind = DecisionPlanKind::BehaviorTree,
                                                   .requiredCapabilities = AiCapabilitySet::Of(AiCapability::Behavior),
                                                   .schemaVersion = CurrentAiSceneComponentSchemaVersion,
                                                   .startupPolicy = AiStartupPolicy::OnSceneActivation,
                                                   .enabled = true};

            CHECK(ValidateAiAgentComponent(agent).HasValue());
            CHECK(ValidateAiControllerComponent(controller).HasValue());
            CHECK(ValidateAiSceneComponents(std::array{AiSceneComponentView{.agent = &agent, .controller = &controller}}).HasValue());

            const auto missingDescriptor =
                ValidateAiSceneComponents(std::array{AiSceneComponentView{.agent = nullptr, .controller = &controller}});
            CHECK(missingDescriptor.HasValue());

            AiControllerComponent invalid = controller;
            invalid.requiredCapabilities.bits |= 1U << 31U;
            ExpectError(ValidateAiControllerComponent(invalid), AIErrors::SceneComponentInvalid);

            invalid = controller;
            invalid.startupPolicy = static_cast<AiStartupPolicy>(255);
            ExpectError(ValidateAiControllerComponent(invalid), AIErrors::SceneComponentInvalid);
        }

        TEST_CASE("AI scene validation rejects duplicate authored identities and preserves descriptor diagnostics", "[unit][ai][scene]") {
            const AiAgentComponent first{.agent = MakeIdentity<AgentId>(8)};
            const AiAgentComponent duplicate{.agent = first.agent};
            const auto duplicateResult =
                ValidateAiSceneComponents(std::array{AiSceneComponentView{.agent = &first, .controller = nullptr},
                                                     AiSceneComponentView{.agent = &duplicate, .controller = nullptr}});
            ExpectError(duplicateResult, AIErrors::DescriptorConflict);

            const auto schema = MakeSchema(20);
            const auto plan = MakePlan(schema, 30);
            auto descriptor = MakeDescriptor(schema, plan, 40);
            CHECK(ValidateAiControllerDescriptor(descriptor).HasValue());

            descriptor.decisionAsset = MakeIdentity<DecisionGraphAssetId>(41);
            ExpectError(ValidateAiControllerDescriptor(descriptor), AIErrors::ControllerDescriptorIncompatible);
        }

        TEST_CASE("AI controller bindings require exact admitted descriptor identities", "[unit][ai][scene]") {
            const auto schema = MakeSchema(50);
            const auto plan = MakePlan(schema, 60);
            const auto descriptor = MakeDescriptor(schema, plan, 70);
            const AiControllerComponent component{.controller = descriptor.controller,
                                                  .decisionAsset = descriptor.decisionAsset,
                                                  .blackboardSchema = schema->Identity(),
                                                  .decisionKind = descriptor.decisionKind,
                                                  .requiredCapabilities = AiCapabilitySet::Of(AiCapability::Perception),
                                                  .schemaVersion = CurrentAiSceneComponentSchemaVersion,
                                                  .startupPolicy = AiStartupPolicy::OnSceneActivation,
                                                  .enabled = true};
            CHECK(ValidateAiControllerBinding(component, descriptor).HasValue());

            AiControllerComponent mismatched = component;
            mismatched.blackboardSchema = MakeIdentity<BlackboardSchemaId>(999);
            ExpectError(ValidateAiControllerBinding(mismatched, descriptor), AIErrors::ControllerDescriptorIncompatible);
        }
    }  // namespace
}  // namespace Horo::AI
