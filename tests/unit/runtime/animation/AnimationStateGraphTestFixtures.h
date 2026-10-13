#pragma once

#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationGraphSource.h"

namespace Horo::Animation::Test::StateGraph {
    /** @brief A real compiled two-clip state node, with a scalar locomotion gate and a queued trigger interrupt. */
    inline AnimationGraphData Data() {
        auto graph = Graph::Simple();
        graph.parameters = {{Id<GraphParameterId>(1), "speed", GraphValueType::Float, 0.0F},
                            {Id<GraphParameterId>(2), "interrupt", GraphValueType::Trigger, false}};
        GraphStateMachineNode machine;
        machine.initial = Id<GraphStateId>(10);
        machine.states = {{Id<GraphStateId>(10), Asset<AnimationClipId>(3)},
                          {Id<GraphStateId>(20), Asset<AnimationClipId>(4)},
                          {Id<GraphStateId>(30), Asset<AnimationClipId>(3)}};
        machine.transitions = {{Id<GraphTransitionId>(100),
                                Id<GraphStateId>(10),
                                Id<GraphStateId>(20),
                                1,
                                {100},
                                {},
                                GraphStateInterruption::Either,
                                GraphStateBlendCurve::Linear,
                                {{Id<GraphParameterId>(1), GraphStatePredicate::Above, 1.0F, 0.5F}}},
                               {Id<GraphTransitionId>(200),
                                Id<GraphStateId>(20),
                                Id<GraphStateId>(30),
                                2,
                                {50},
                                {},
                                GraphStateInterruption::None,
                                GraphStateBlendCurve::Linear,
                                {{Id<GraphParameterId>(2), GraphStatePredicate::Trigger, true}}}};
        graph.definitions[0].nodes[0].payload = std::move(machine);
        return graph;
    }

    inline GraphStateMachineNode &Machine(AnimationGraphData &graph) {
        return std::get<GraphStateMachineNode>(graph.definitions.front().nodes.front().payload);
    }

    /** @brief Install the exact nested machine definition used by reentry and old-subtree retirement regressions. */
    inline AnimationGraphData AddNestedMachine(AnimationGraphData graph, const GraphStateMachineNode &child) {
        auto node = Graph::Clip(4);
        node.payload = child;
        graph.definitions.push_back({Id<GraphDefinitionId>(2), {}, {node, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
        return graph;
    }

    /** @brief Executes actual Evaluate/Commit and copies the resulting pose and event block into caller-owned storage. */
    struct Tick final {
        GraphInstanceOutcome outcome;
        std::array<Math::Transform, 1> pose;
        std::array<GraphStateEvent, 262> events;
        std::uint32_t count{};

        Tick(AnimationGraphInstance &instance, const std::uint64_t tick, const std::int64_t delta,
             const std::span<const GraphParameterInput> parameters = {}, const GraphPlaybackRate rate = {}) {
            REQUIRE(instance.Evaluate({Id<AnimationTickId>(tick), {delta}, rate, parameters}).HasValue());
            outcome = instance.Commit(Id<AnimationTickId>(tick));
            REQUIRE(outcome.HasValue());
            REQUIRE(instance.CopyPose(outcome.publication.currentPose, pose).HasValue());
            REQUIRE(instance.CopyStateEvents(outcome.publication.currentPose, events, count).HasValue());
        }
    };

    /** @brief Owns the prepared nested graph, live instance and its committed two-owner initial entry together. */
    struct NestedFixture final {
        Instance::Prepared prepared;
        AnimationGraphInstance instance;
        Tick entered;

        NestedFixture(AnimationGraphData graph, const GraphStateMachineNode &child)
            : prepared(AddNestedMachine(std::move(graph), child)), instance(prepared.Create()), entered(instance, 1, 10) {
            REQUIRE(entered.count == 2);
        }
    };
}  // namespace Horo::Animation::Test::StateGraph
