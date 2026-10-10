#include "AnimationGraphTestFixtures.h"

using namespace Horo;
using namespace Horo::Animation;
using namespace Horo::Animation::Test;
using namespace Horo::Animation::Test::Graph;

namespace {
    /** @brief Checks distinct state and slots for two calls to one shared clip definition. */
    void CheckRepeatedClipPlayers(const AnimationGraphProgram &program) {
        std::vector<const GraphInstructionOccurrence *> players;
        for (const auto &occurrence : program.Occurrences())
            if (occurrence.clipPlayer)
                players.push_back(&occurrence);
        REQUIRE(players.size() == 2);
        CHECK(players[0]->source == players[1]->source);
        CHECK(players[0]->callPath != players[1]->callPath);
        CHECK(players[0]->callPath.front().node == Id<GraphNodeId>(4));
        CHECK(players[1]->callPath.front().node == Id<GraphNodeId>(5));
        CHECK(players[0]->clipPlayer != players[1]->clipPlayer);
        CHECK(players[0]->output != players[1]->output);
        CHECK(players[0]->clipBindingIndex == players[1]->clipBindingIndex);
    }
}  // namespace

TEST_CASE("Graph repeated subgraph calls have stable distinct state and access plans", "[animation][graph]") {
    const auto data = RepeatedCalls();
    AnimationGraphCompileContext context;
    context.limits.poseSlots = 3;
    context.limits.scalarSlots = 1;
    context.limits.clipPlayers = 2;
    context.limits.poseTransforms = 3;
    context.limits.sourceMapCallEntries = 4;
    context.limits.evaluationInstructions = 9;
    context.limits.callDepth = 2;
    const auto result = CompileGraph(data, context);
    REQUIRE(result.HasValue());
    const auto &program = result.Value();
    REQUIRE(program.Occurrences().size() == 9);
    CheckRepeatedClipPlayers(program);
    CHECK(program.Memory().poseTransforms == 3);
    CHECK(program.OutputSlot().type == GraphValueType::Pose);
    CHECK(program.Memory().clipPlayers == 2);
    CHECK(program.Memory().workingSlots[1] == 1);
    CheckWorkingAccessPlan(program);
    SECTION("pose capacity") {
        context.limits.poseSlots = 2;
    }
    SECTION("scalar capacity") {
        context.limits.scalarSlots = 0;
    }
    SECTION("player capacity") {
        context.limits.clipPlayers = 1;
    }
    SECTION("transform capacity") {
        context.limits.poseTransforms = 2;
    }
    SECTION("source path capacity") {
        context.limits.sourceMapCallEntries = 3;
    }
    SECTION("expanded work capacity") {
        context.limits.evaluationInstructions = 8;
    }
    Fails(data, AnimationErrors::GraphLimitExceeded, context);
}

TEST_CASE("Graph compilation rejects missing extra incompatible or stale exact dependencies", "[animation][graph]") {
    const GraphTestDependencies dependencies;
    auto context = dependencies.Context();
    auto data = Simple();
    auto extra = dependencies.clips;
    const auto good = AnimationGraphProgram::Compile(data, context);
    REQUIRE(good.HasValue());
    CHECK(good.Value().SkeletonBinding().generation == context.skeletonGeneration);
    CHECK(good.Value().ClipBindings().front() == dependencies.clips.front().Data().descriptor);
    SECTION("extra duplicate clip") {
        extra.push_back(extra.front());
        context.clips = extra;
    }
    SECTION("missing skeleton") {
        context.skeleton = nullptr;
    }
    SECTION("missing clip") {
        context.clips = {};
    }
    SECTION("stale skeleton generation") {
        context.skeletonGeneration = Id<SkeletonAssetGeneration>(9);
    }
    SECTION("wrong skeleton identity") {
        data.skeleton = Asset<SkeletonId>(9);
    }
    SECTION("wrong clip identity") {
        std::get<GraphClipNode>(data.definitions.front().nodes.front().payload).clip = Asset<AnimationClipId>(9);
    }
    const auto failed = AnimationGraphProgram::Compile(data, context);
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == AnimationErrors::GraphBindingMismatch.code.Value());
    CHECK(good.Value().SkeletonBinding().generation == Id<SkeletonAssetGeneration>(5));
}

TEST_CASE("Graph nested interfaces preserve actual writers and complete calls after their callees", "[animation][graph]") {
    auto data = Blended();
    auto &root = data.definitions.front();
    root.nodes[1] = Call(5, 2, true);
    root.connections.push_back(Edge(4, 1, 5, 2));
    const GraphNode input{Id<GraphNodeId>(8),
                          GraphInputNode{Id<GraphInterfaceId>(7)},
                          {Pin(1, GraphPinRole::Result, GraphValueType::Pose, true)}};
    data.definitions.push_back({Id<GraphDefinitionId>(2),
                                {{Id<GraphInterfaceId>(7), GraphValueType::Pose}},
                                {input, Call(9, 3, true), Output(2)},
                                {Edge(8, 1, 9, 2), Edge(9, 1, 2, 1)}});
    data.definitions.push_back(
        {Id<GraphDefinitionId>(3), {{Id<GraphInterfaceId>(7), GraphValueType::Pose}}, {input, Output(2)}, {Edge(8, 1, 2, 1)}});
    GraphSourceLocation expectedWriter{Id<GraphDefinitionId>(1), Id<GraphNodeId>(4), {}};
    SECTION("nested passthrough aliases the nonzero caller writer") {}
    SECTION("nested Blend publishes its actual completed writer") {
        auto &callee = data.definitions.back();
        callee.nodes = {input, Clip(4), Parameter(3), Blend(6), Output(2)};
        callee.connections = {Edge(8, 1, 6, 1), Edge(4, 1, 6, 2), Edge(3, 1, 6, 3, GraphValueType::Float), Edge(6, 4, 2, 1)};
        expectedWriter = {Id<GraphDefinitionId>(3), Id<GraphNodeId>(6), {}};
    }
    const auto result = CompileGraph(data);
    REQUIRE(result.HasValue());
    const auto &program = result.Value();
    const auto occurrences = program.Occurrences();
    CheckWorkingAccessPlan(program);
    std::size_t calls{}, interfaces{};
    for (std::size_t index = 0; index < occurrences.size(); ++index) {
        const auto &occurrence = occurrences[index];
        const auto &instruction = program.Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex];
        if (std::holds_alternative<GraphInputNode>(instruction.payload)) {
            ++interfaces;
            REQUIRE(occurrence.inputs.size() == 1);
            CHECK(occurrence.inputs.front().sourceOccurrence != 0);
            CHECK(occurrences[occurrence.inputs.front().sourceOccurrence].source.definition == Id<GraphDefinitionId>(1));
            CHECK(occurrences[occurrence.inputs.front().sourceOccurrence].source.node == Id<GraphNodeId>(4));
        }
        if (std::holds_alternative<GraphCallNode>(instruction.payload)) {
            ++calls;
            CHECK(occurrences[occurrence.outputProducer].source == expectedWriter);
            CheckCallCompletionOrder(occurrences, index);
        }
    }
    CHECK(calls == 2);
    CHECK(interfaces == 2);
}
