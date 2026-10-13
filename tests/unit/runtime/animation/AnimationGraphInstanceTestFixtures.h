#pragma once

#include "AnimationGraphTestFixtures.h"
#include "Horo/Animation/AnimationGraphInstance.h"

namespace Horo::Animation::Test::Instance {
    /** @brief Actual compiler, skeleton and sampled-clip fixture; no synthetic executable plan. */
    struct Prepared final {
        std::shared_ptr<const SkeletonAsset> skeleton;
        std::vector<AnimationClipAsset> compileClips;
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        std::shared_ptr<const AnimationGraphProgram> program;
        GraphInstanceCreateContext context;

        explicit Prepared(AnimationGraphData graph = Graph::Simple(), std::uint64_t clipGeneration = 6) {
            skeleton = std::make_shared<const SkeletonAsset>(Graph::GraphTestDependencies::MakeSkeleton());
            for (std::uint8_t id = 3; id <= 4; ++id) {
                AnimationClipData data;
                data.descriptor = {.id = Asset<AnimationClipId>(id),
                                   .generation = Id<AnimationClipGeneration>(clipGeneration),
                                   .skeleton = skeleton->Data().skeleton,
                                   .skeletonGeneration = Id<SkeletonAssetGeneration>(5),
                                   .duration = {100},
                                   .wrapMode = AnimationWrapMode::Loop};
                Math::Transform first;
                first.translation.x = id == 3 ? 0.0F : 100.0F;
                auto last = first;
                last.translation.x += 100.0F;
                data.tracks = {{Id<JointId>(1), {{{0}, first}, {{100}, last}}}};
                auto clip = AnimationClipAsset::Create(std::move(data), *skeleton);
                REQUIRE(clip.HasValue());
                compileClips.push_back(std::move(clip).Value());
            }
            bool secondClip{};
            for (const auto &definition : graph.definitions)
                for (const auto &node : definition.nodes) {
                    if (const auto *clip = std::get_if<GraphClipNode>(&node.payload); clip && clip->clip == Asset<AnimationClipId>(4))
                        secondClip = true;
                    if (const auto *machine = std::get_if<GraphStateMachineNode>(&node.payload))
                        for (const auto &state : machine->states)
                            if (const auto *clip = std::get_if<AnimationClipId>(&state.source); clip && *clip == Asset<AnimationClipId>(4))
                                secondClip = true;
                }
            if (!secondClip)
                compileClips.erase(compileClips.begin() + 1);
            for (const auto &clip : compileClips)
                clips.push_back(std::make_shared<const AnimationClipAsset>(clip));
            AnimationGraphCompileContext compile;
            compile.skeleton = skeleton.get();
            compile.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
            compile.clips = compileClips;
            auto compiled = AnimationGraphProgram::Compile(graph, compile);
            REQUIRE(compiled.HasValue());
            program = std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value());
            context.instance = {Id<AnimationRuntimeId>(1), Id<AnimationComponentId>(2), {0, 1}};
        }

        [[nodiscard]] GraphInstanceAssets Assets() const {
            return {program, skeleton, Id<SkeletonAssetGeneration>(5), clips};
        }

        [[nodiscard]] AnimationGraphInstance Create() const {
            auto result = AnimationGraphInstance::Create(Assets(), context);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    };

    inline AnimationGraphData Blended() {
        auto graph = Graph::Blended();
        std::get<GraphClipNode>(graph.definitions.front().nodes[1].payload).clip = Asset<AnimationClipId>(4);
        return graph;
    }

    /** @brief Declares every scalar variant, optionally adding deliberately disconnected typed readers. */
    inline AnimationGraphData AllScalarTypes(const bool disconnectedReaders = false) {
        auto graph = Blended();
        graph.parameters.push_back({Id<GraphParameterId>(2), "enabled", GraphValueType::Boolean, false});
        graph.parameters.push_back({Id<GraphParameterId>(3), "index", GraphValueType::Integer, std::int32_t{0}});
        graph.parameters.push_back({Id<GraphParameterId>(4), "trigger", GraphValueType::Trigger, false});
        if (disconnectedReaders) {
            for (const auto &parameter : graph.parameters) {
                if (parameter.type == GraphValueType::Float)
                    continue;
                auto node = Graph::Parameter(10 + parameter.id.Value(), parameter.type);
                std::get<GraphParameterNode>(node.payload).parameter = parameter.id;
                graph.definitions.front().nodes.push_back(std::move(node));
            }
        }
        return graph;
    }
}  // namespace Horo::Animation::Test::Instance
