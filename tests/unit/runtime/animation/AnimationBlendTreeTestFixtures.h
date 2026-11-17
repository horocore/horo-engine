#pragma once
#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <array>

namespace Horo::Animation::Test::BlendTree {
    /** @brief A real two-clip linear tree in the canonical graph namespace. */
    inline GraphBlendTreeNode Tree() {
        GraphBlendTreeNode tree;
        tree.x = Id<GraphParameterId>(1);
        tree.samples = {{Id<GraphBlendSampleId>(10), Asset<AnimationClipId>(3), 0, 0},
                        {Id<GraphBlendSampleId>(20), Asset<AnimationClipId>(4), 1, 0}};
        return tree;
    }

    /** @brief One tree replaces a real clip writer without changing the canonical output connection. */
    inline AnimationGraphData Data(GraphBlendTreeNode tree = Tree()) {
        auto graph = Graph::Simple();
        graph.parameters = {{Id<GraphParameterId>(1), "x", GraphValueType::Float, 0.5F},
                            {Id<GraphParameterId>(2), "y", GraphValueType::Float, 0.25F}};
        graph.definitions[0].nodes[0].payload = std::move(tree);
        return graph;
    }

    /** @brief An authored triangle with genuine distinct clip publications at its vertices. */
    inline GraphBlendTreeNode Triangle() {
        auto tree = Tree();
        tree.dimension = GraphBlendDimension::Two;
        tree.y = Id<GraphParameterId>(2);
        tree.samples.push_back({Id<GraphBlendSampleId>(30), Asset<AnimationClipId>(5), 0, 1});
        tree.triangles = {{{Id<GraphBlendSampleId>(10), Id<GraphBlendSampleId>(20), Id<GraphBlendSampleId>(30)}}};
        return tree;
    }

    /** @brief Collects exact direct/state/tree clip references without inventing an executable access plan. */
    inline bool Uses(const AnimationGraphData &graph, const AnimationClipId clip) {
        for (const auto &definition : graph.definitions)
            for (const auto &node : definition.nodes) {
                if (Instance::ReferencesClip(node.payload, clip))
                    return true;
                if (const auto *tree = std::get_if<GraphBlendTreeNode>(&node.payload))
                    for (const auto &sample : tree->samples)
                        if (sample.clip == clip)
                            return true;
            }
        return false;
    }

    /** @brief Actual immutable clips have unequal durations, known translation tracks and optional semantic markers. */
    inline AnimationClipData ClipData(const std::uint8_t id, const bool markers) {
        AnimationClipData data;
        const auto duration = 100 * (id - 2);
        data.descriptor = {.id = Asset<AnimationClipId>(id),
                           .generation = Id<AnimationClipGeneration>(6),
                           .skeleton = Asset<SkeletonId>(2),
                           .skeletonGeneration = Id<SkeletonAssetGeneration>(5),
                           .duration = {duration},
                           .wrapMode = AnimationWrapMode::Loop};
        Math::Transform first;
        first.translation.x = 100.0F * (id - 3);
        auto last = first;
        last.translation.x += 100;
        data.tracks = {{Id<JointId>(1), {{{0}, first}, {{duration}, last}}}};
        if (markers)
            data.syncMarkers = {{Id<AnimationSyncMarkerId>(1), {0}}, {Id<AnimationSyncMarkerId>(2), {duration * (id == 3 ? 2 : 3) / 4}}};
        return data;
    }

    /** @brief Owns real dependencies and compiler output; failed candidates can be tested before any instance exists. */
    struct Prepared final {
        std::shared_ptr<const SkeletonAsset> skeleton{std::make_shared<const SkeletonAsset>(Graph::GraphTestDependencies::MakeSkeleton())};
        std::vector<AnimationClipAsset> compileClips;
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        GraphInstanceCreateContext context;

        explicit Prepared(const AnimationGraphData &graph = Data(), const bool markers = false) {
            for (std::uint8_t id = 3; id <= 5; ++id) {
                if (!Uses(graph, Asset<AnimationClipId>(id)))
                    continue;
                auto result = AnimationClipAsset::Create(ClipData(id, markers), *skeleton);
                REQUIRE(result.HasValue());
                compileClips.push_back(std::move(result).Value());
            }
            context.instance = {Id<AnimationRuntimeId>(1), Id<AnimationComponentId>(2), {0, 1}};
            Refresh();
        }

        void Refresh() {
            clips.clear();
            for (const auto &clip : compileClips)
                clips.push_back(std::make_shared<const AnimationClipAsset>(clip));
        }

        [[nodiscard]] AnimationGraphCompileContext CompileContext() const {
            AnimationGraphCompileContext result;
            result.skeleton = skeleton.get();
            result.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
            result.clips = compileClips;
            return result;
        }

        [[nodiscard]] Result<AnimationGraphProgram> Compile(const AnimationGraphData &graph) const {
            return AnimationGraphProgram::Compile(graph, CompileContext());
        }

        [[nodiscard]] GraphInstanceAssets Assets(std::shared_ptr<const AnimationGraphProgram> program) const {
            return {std::move(program), skeleton, Id<SkeletonAssetGeneration>(5), clips};
        }

        [[nodiscard]] AnimationGraphInstance Create(const AnimationGraphData &graph = Data()) const {
            auto compiled = Compile(graph);
            REQUIRE(compiled.HasValue());
            auto result =
                AnimationGraphInstance::Create(Assets(std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value())), context);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    };

    /** @brief Fixed caller-owned copies of a complete real committed pose/member publication. */
    struct Tick final {
        std::array<Math::Transform, 1> pose;
        std::array<GraphBlendMemberObservation, 128> members;
        std::uint32_t count{};
        GraphInstanceOutcome outcome;

        Tick(AnimationGraphInstance &instance, const std::uint64_t tick, const std::int64_t delta,
             const std::span<const GraphParameterInput> parameters = {}, const GraphPlaybackRate rate = {}) {
            REQUIRE(instance.Evaluate({Id<AnimationTickId>(tick), {delta}, rate, parameters}).HasValue());
            outcome = instance.Commit(Id<AnimationTickId>(tick));
            REQUIRE(outcome.HasValue());
            REQUIRE(instance.CopyPose(outcome.publication.currentPose, pose).HasValue());
            REQUIRE(instance.CopyBlendMembers(outcome.publication.currentPose, members, count).HasValue());
        }
    };

    /** @brief Changes authored coordinates through real typed invocation inputs. */
    inline std::array<GraphParameterInput, 2> Coordinates(const float x, const float y = 0.25F) {
        return {{{Id<GraphParameterId>(1), x}, {Id<GraphParameterId>(2), y}}};
    }
}  // namespace Horo::Animation::Test::BlendTree
