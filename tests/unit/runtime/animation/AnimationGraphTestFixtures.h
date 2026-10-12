#pragma once
#include "AnimationTestFixtures.h"
#include "Horo/Animation/AnimationGraph.h"

#include <algorithm>

namespace Horo::Animation::Test::Graph {
    inline GraphPin Pin(std::uint64_t id, GraphPinRole role, GraphValueType type, bool output, std::uint64_t interfaceId = 0) {
        return {Id<GraphPinId>(id), role, type, output, interfaceId == 0 ? GraphInterfaceId{} : Id<GraphInterfaceId>(interfaceId)};
    }

    inline GraphNode Clip(std::uint64_t id) {
        return {Id<GraphNodeId>(id), GraphClipNode{Asset<AnimationClipId>(3)}, {Pin(1, GraphPinRole::Result, GraphValueType::Pose, true)}};
    }

    inline GraphNode Output(std::uint64_t id) {
        return {Id<GraphNodeId>(id), GraphOutputNode{}, {Pin(1, GraphPinRole::Value, GraphValueType::Pose, false)}};
    }

    inline GraphNode Parameter(std::uint64_t id, GraphValueType type = GraphValueType::Float) {
        return {Id<GraphNodeId>(id), GraphParameterNode{Id<GraphParameterId>(1)}, {Pin(1, GraphPinRole::Result, type, true)}};
    }

    inline GraphNode Blend(std::uint64_t id) {
        return {Id<GraphNodeId>(id),
                GraphBlendNode{},
                {Pin(1, GraphPinRole::FirstPose, GraphValueType::Pose, false),
                 Pin(2, GraphPinRole::SecondPose, GraphValueType::Pose, false), Pin(3, GraphPinRole::Weight, GraphValueType::Float, false),
                 Pin(4, GraphPinRole::Result, GraphValueType::Pose, true)}};
    }

    inline GraphConnection Edge(std::uint64_t source, std::uint64_t sourcePin, std::uint64_t destination, std::uint64_t destinationPin,
                                GraphValueType type = GraphValueType::Pose) {
        return {{Id<GraphNodeId>(source), Id<GraphPinId>(sourcePin)}, {Id<GraphNodeId>(destination), Id<GraphPinId>(destinationPin)}, type};
    }

    inline AnimationGraphData Simple() {
        AnimationGraphData data;
        data.id = Asset<AnimationGraphId>(1);
        data.skeleton = Asset<SkeletonId>(2);
        data.entry = Id<GraphDefinitionId>(1);
        data.definitions.push_back({data.entry, {}, {Clip(4), Output(2)}, {Edge(4, 1, 2, 1)}});
        return data;
    }

    inline AnimationGraphData Blended() {
        auto data = Simple();
        data.parameters.push_back({Id<GraphParameterId>(1), "weight", GraphValueType::Float, 0.5F});
        auto &definition = data.definitions.front();
        definition.nodes = {Clip(4), Clip(5), Parameter(3), Blend(6), Output(2)};
        definition.connections = {Edge(4, 1, 6, 1), Edge(5, 1, 6, 2), Edge(3, 1, 6, 3, GraphValueType::Float), Edge(6, 4, 2, 1)};
        return data;
    }

    inline GraphNode Call(std::uint64_t node, std::uint64_t target, bool withInput = false) {
        GraphNode value{Id<GraphNodeId>(node),
                        GraphCallNode{Id<GraphDefinitionId>(target)},
                        {Pin(1, GraphPinRole::Result, GraphValueType::Pose, true)}};
        if (withInput)
            value.pins.push_back(Pin(2, GraphPinRole::Interface, GraphValueType::Pose, false, 7));
        return value;
    }

    inline AnimationGraphData Subgraphs() {
        auto data = Simple();
        auto &root = data.definitions.front();
        root.nodes = {Clip(4), Call(5, 2, true), Output(2)};
        root.connections = {Edge(4, 1, 5, 2), Edge(5, 1, 2, 1)};
        data.definitions.push_back(
            {Id<GraphDefinitionId>(2),
             {{Id<GraphInterfaceId>(7), GraphValueType::Pose}},
             {{Id<GraphNodeId>(8), GraphInputNode{Id<GraphInterfaceId>(7)}, {Pin(1, GraphPinRole::Result, GraphValueType::Pose, true)}},
              Output(2)},
             {Edge(8, 1, 2, 1)}});
        return data;
    }

    /** @brief Creates two distinct calls to the same immutable clip subprogram. */
    inline AnimationGraphData RepeatedCalls() {
        auto data = Blended();
        data.definitions.front().nodes[0] = Call(4, 2);
        data.definitions.front().nodes[1] = Call(5, 2);
        data.definitions.push_back({Id<GraphDefinitionId>(2), {}, {Clip(4), Output(2)}, {Edge(4, 1, 2, 1)}});
        return data;
    }

    /** @brief Requires physical writers and preserves their exact slots through every alias and operand. */
    inline void CheckWorkingAccessPlan(const AnimationGraphProgram &program) {
        const auto occurrences = program.Occurrences();
        for (std::size_t index = 0; index < occurrences.size(); ++index) {
            const auto &occurrence = occurrences[index];
            const auto &payload = program.Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload;
            const bool writes = std::holds_alternative<GraphClipNode>(payload) || std::holds_alternative<GraphBlendNode>(payload) ||
                                std::holds_alternative<GraphParameterNode>(payload);
            REQUIRE(occurrence.outputProducer <= index);
            CHECK((occurrence.outputProducer == index) == writes);
            CHECK(occurrences[occurrence.outputProducer].output == occurrence.output);
            for (const auto &access : occurrence.inputs) {
                REQUIRE(access.sourceOccurrence < index);
                const auto &producer = occurrences[access.sourceOccurrence];
                CHECK(producer.outputProducer == access.sourceOccurrence);
                CHECK(producer.output == access.slot);
            }
        }
    }

    /** @brief Requires the entire exact callee occurrence range to precede its call completion. */
    inline void CheckCallCompletionOrder(std::span<const GraphInstructionOccurrence> occurrences, std::size_t index) {
        const auto &occurrence = occurrences[index];
        bool hasCallee{};
        for (std::size_t child = 0; child < occurrences.size(); ++child) {
            const auto &path = occurrences[child].callPath;
            if (path.size() > occurrence.callPath.size() &&
                std::equal(occurrence.callPath.begin(), occurrence.callPath.end(), path.begin()) &&
                path[occurrence.callPath.size()] == occurrence.source) {
                hasCallee = true;
                CHECK(child < index);
            }
        }
        CHECK(hasCallee);
    }

    struct GraphTestDependencies final {
        SkeletonAsset skeleton;
        std::vector<AnimationClipAsset> clips;

        static SkeletonAsset MakeSkeleton() {
            auto created = SkeletonAsset::Create({.skeleton = Asset<SkeletonId>(2), .joints = {{.id = Id<JointId>(1), .name = "root"}}});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        GraphTestDependencies() : skeleton(MakeSkeleton()) {
            AnimationClipData data;
            data.descriptor.id = Asset<AnimationClipId>(3);
            data.descriptor.generation = Id<AnimationClipGeneration>(6);
            data.descriptor.skeleton = skeleton.Data().skeleton;
            data.descriptor.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
            data.descriptor.duration = {100};
            data.tracks = {{Id<JointId>(1), {{{0}, {}}, {{100}, {}}}}};
            auto created = AnimationClipAsset::Create(std::move(data), skeleton);
            REQUIRE(created.HasValue());
            clips.push_back(std::move(created).Value());
        }

        AnimationGraphCompileContext Context() const {
            AnimationGraphCompileContext context;
            context.skeleton = &skeleton;
            context.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
            context.clips = clips;
            return context;
        }
    };

    inline Result<AnimationGraphProgram> CompileGraph(const AnimationGraphData &data, AnimationGraphCompileContext context = {}) {
        static const GraphTestDependencies dependencies;
        context.skeleton = &dependencies.skeleton;
        context.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
        context.clips = dependencies.clips;
        return AnimationGraphProgram::Compile(data, context);
    }

    inline void Fails(const AnimationGraphData &data, const ErrorCodeDescriptor &error, const AnimationGraphCompileContext &context = {}) {
        const auto result = CompileGraph(data, context);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }
}  // namespace Horo::Animation::Test::Graph
