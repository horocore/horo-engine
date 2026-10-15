#pragma once
#include "../runtime/ai/AiSceneTestSupport.h"
#include "Horo/Gameplay/GameplayAiContext.h"

namespace Horo::Gameplay::AiTestSupport {
    using AI::TestSupport::ExpectError;
    using AI::TestSupport::MakeIdentity;

    /** @brief Real scene-owned tasks and blackboard with two inert native/script declarations. */
    struct Fixture final {
        AI::AiSceneRuntime runtime{std::move(AI::AiSceneRuntime::Create()).Value()};
        AI::TestSupport::ActivationFixture scene{AI::TestSupport::MakeFixture(1, 11, 7, 0)};
        std::unique_ptr<AI::AiSceneActivationCandidate> publication;
        AI::AiCapabilityDescriptorSet declarations;
        std::shared_ptr<GameplayAiContext> context;
        AI::AgentHandle agent;

        explicit Fixture(const AI::BlackboardValue &value = AI::BlackboardScalarValue{false}) {
            auto key = scene.schema->Keys().front();
            if (const auto *scalar = std::get_if<AI::BlackboardScalarValue>(&value))
                key.kind = static_cast<AI::BlackboardValueKind>(scalar->index());
            else if (const auto *collection = std::get_if<AI::BlackboardCollectionValue>(&value)) {
                key.kind = collection->elementKind;
                key.cardinality = AI::BlackboardValueCardinality::Collection;
                key.maximumCollectionElements = AI::MaximumBlackboardCollectionElements;
            }
            key.defaultValue = value;
            auto schema = AI::BlackboardSchema::Capture({.identity = scene.schema->Identity(), .version = 1, .keys = std::array{key}});
            REQUIRE(schema.HasValue());
            scene.schema = std::make_shared<const AI::BlackboardSchema>(std::move(schema).Value());
            scene.plan = AI::TestSupport::MakePlan(scene.schema, 71);
            scene.descriptor.blackboardSchema = scene.schema;
            scene.descriptor.decisionPlan = scene.plan;
            publication = AI::TestSupport::Publish(runtime, scene);
            AI::AiCapabilityDescriptorRegistry registry;
            REQUIRE(registry.Register({MakeIdentity<AI::TaskId>(41), scene.schema->Identity(), AI::AiTaskAuthoring::Native}).HasValue());
            REQUIRE(registry.Register({MakeIdentity<AI::TaskId>(42), scene.schema->Identity(), AI::AiTaskAuthoring::Script}).HasValue());
            declarations = registry.Freeze();
            auto created = GameplayAiContext::Create({"com.example.module", true, true}, runtime.CapabilityAtSafePoint(), declarations);
            REQUIRE(created.HasValue());
            context = std::move(created).Value();
            auto resolved = context->ResolveAgent(scene.agent.agent.agent);
            REQUIRE(resolved.HasValue());
            agent = resolved.Value();
        }
    };
}  // namespace Horo::Gameplay::AiTestSupport
