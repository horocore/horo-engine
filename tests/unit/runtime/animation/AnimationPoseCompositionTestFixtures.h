#pragma once
/** @file AnimationPoseCompositionTestFixtures.h
 * @brief Shared real pose-composition compiler/runtime fixtures for layer and state integration tests.
 */
#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <cmath>
#include <format>
#include <nlohmann/json.hpp>

namespace Horo::Animation::Test::PoseComposition {
    using namespace Graph;

    /** @brief Creates real source graph operands for one explicit additive layer. */
    inline AnimationGraphData Layer(const GraphBlendMode mode = GraphBlendMode::Normalized) {
        auto graph = Instance::Blended();
        auto &definition = graph.definitions.front();
        auto &blend = std::get<GraphBlendNode>(definition.nodes[3].payload);
        blend.mode = mode;
        if (mode != GraphBlendMode::Normalized) {
            definition.nodes[3].pins.push_back(Pin(5, GraphPinRole::ReferencePose, GraphValueType::Pose, false));
            auto reference = Clip(7);
            std::get<GraphClipNode>(reference.payload).clip = Asset<AnimationClipId>(5);
            definition.nodes.push_back(std::move(reference));
            definition.connections.push_back(Edge(7, 1, 6, 5));
        }
        return graph;
    }

    /** @brief Builds an additive clip opcode with separate actual base/reference producers. */
    inline AnimationGraphData AdditiveClipGraph() {
        auto source = Simple();
        auto &definition = source.definitions.front();
        auto &additive = std::get<GraphClipNode>(definition.nodes[0].payload);
        additive.clip = Asset<AnimationClipId>(4);
        additive.referencePose = Id<AnimationReferencePoseId>(44);
        definition.nodes[0].pins.push_back(Pin(2, GraphPinRole::FirstPose, GraphValueType::Pose, false));
        definition.nodes[0].pins.push_back(Pin(3, GraphPinRole::ReferencePose, GraphValueType::Pose, false));
        definition.nodes.push_back(Clip(8));
        auto reference = Clip(9);
        std::get<GraphClipNode>(reference.payload).clip = Asset<AnimationClipId>(5);
        definition.nodes.push_back(std::move(reference));
        definition.connections.push_back(Edge(8, 1, 4, 2));
        definition.connections.push_back(Edge(9, 1, 4, 3));
        return source;
    }

    /** @brief Verifies exact canonical codec/migration values without duplicating test assertions. */
    inline AnimationGraphData RoundTrip(const AnimationGraphData &source) {
        const auto bytes = SerializeAnimationGraphSource(source);
        REQUIRE(bytes.HasValue());
        const auto decoded = DeserializeAnimationGraphSource(bytes.Value(), source.id);
        REQUIRE(decoded.HasValue());
        const auto canonical = MigrateAnimationGraph(source);
        REQUIRE(canonical.HasValue());
        CHECK(decoded.Value() == canonical.Value());
        return decoded.Value();
    }

    /** @brief Exact immutable three-joint dependencies and actual compiled runtime instance. */
    struct LayerFixture final {
        AnimationGraphData graph;
        std::shared_ptr<const SkeletonAsset> skeleton;
        std::vector<AnimationClipAsset> compileClips;
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        GraphInstanceCreateContext create;

        /** @brief Prepares the intentionally shuffled stable-joint skeleton through real validation. */
        static std::shared_ptr<const SkeletonAsset> MakeSkeleton(const SkeletonId identity) {
            SkeletonAssetData data;
            data.skeleton = identity;
            for (const auto id : {11ULL, 3ULL, 7ULL}) {
                SkeletonJoint joint;
                joint.id = Id<JointId>(id);
                joint.name = std::format("joint{}", id);
                data.joints.push_back(std::move(joint));
            }
            auto builtSkeleton = SkeletonAsset::Create(std::move(data));
            REQUIRE(builtSkeleton.HasValue());
            return std::make_shared<const SkeletonAsset>(std::move(builtSkeleton).Value());
        }

        /** @brief Keeps exactly the graph's declared clip dependencies. */
        bool UsesClip(const AnimationClipId identity) const {
            return std::ranges::any_of(graph.definitions, [identity](const GraphDefinition &definition) {
                return std::ranges::any_of(definition.nodes, [identity](const GraphNode &node) {
                    const auto *clip = std::get_if<GraphClipNode>(&node.payload);
                    return clip && clip->clip == identity;
                });
            });
        }

        /** @brief Supplies known noncommuting rotations and distinct scale/translation references. */
        static Math::Transform SampleTransform(const std::uint8_t id) {
            Math::Transform transform;
            if (id == 3) {
                transform.translation.x = 10.0F;
                transform.scale = {2, 2, 2};
                transform.rotation = Math::Quaternion::FromAxisAngle({1, 0, 0}, Math::Pi * 0.5F);
            } else if (id == 4) {
                transform.translation.x = 20.0F;
                transform.scale = {4, 4, 4};
                transform.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi * 0.5F);
            } else {
                transform.translation.x = 5.0F;
            }
            return transform;
        }

        /** @brief Builds one exact immutable absolute/additive clip with intentionally partial overlay tracks. */
        AnimationClipAsset MakeClip(const std::uint8_t id, const bool additiveClip) const {
            AnimationClipData clip;
            clip.descriptor = {.id = Asset<AnimationClipId>(id),
                               .generation = Id<AnimationClipGeneration>(6),
                               .skeleton = graph.skeleton,
                               .skeletonGeneration = Id<SkeletonAssetGeneration>(5),
                               .duration = {100},
                               .wrapMode = AnimationWrapMode::Loop};
            if (id == 4 && additiveClip) {
                clip.descriptor.kind = AnimationClipKind::Additive;
                clip.descriptor.referencePose =
                    AnimationReferencePoseBinding{Id<AnimationReferencePoseId>(44), Id<AnimationReferencePoseGeneration>(9)};
            }
            for (const auto joint : {3ULL, 7ULL, 11ULL}) {
                if (id == 4 && joint != 3)
                    continue;
                const auto transform = SampleTransform(id);
                clip.tracks.push_back({Id<JointId>(joint), {{{0}, transform}, {{100}, transform}}});
            }
            auto built = AnimationClipAsset::Create(std::move(clip), *skeleton);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        explicit LayerFixture(AnimationGraphData source = Layer(), const bool additiveClip = false) : graph(std::move(source)) {
            skeleton = MakeSkeleton(graph.skeleton);
            for (std::uint8_t id = 3; id <= 5; ++id)
                if (UsesClip(Asset<AnimationClipId>(id)))
                    compileClips.push_back(MakeClip(id, additiveClip));
            RefreshClips();
            create.instance = {Id<AnimationRuntimeId>(1), Id<AnimationComponentId>(2), {0, 1}};
        }

        void RefreshClips() {
            clips.clear();
            for (const auto &clip : compileClips)
                clips.push_back(std::make_shared<const AnimationClipAsset>(clip));
        }

        AnimationGraphCompileContext Context() const {
            AnimationGraphCompileContext context;
            context.skeleton = skeleton.get();
            context.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
            context.clips = compileClips;
            return context;
        }

        Result<AnimationGraphProgram> Compile() const {
            return AnimationGraphProgram::Compile(graph, Context());
        }

        GraphInstanceAssets Assets() const {
            auto compiled = Compile();
            REQUIRE(compiled.HasValue());
            return {std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value()), skeleton, Id<SkeletonAssetGeneration>(5),
                    clips};
        }

        AnimationGraphInstance Create() const {
            auto instance = AnimationGraphInstance::Create(Assets(), create);
            REQUIRE(instance.HasValue());
            return std::move(instance).Value();
        }
    };

    /** @brief Runs an actual fixed-tick transaction and checks its complete copied pose without hot allocation. */
    inline std::array<Math::Transform, 3> Evaluate(AnimationGraphInstance &instance, const float weight, const std::uint64_t tick = 1) {
        const std::array<GraphParameterInput, 1> parameters{{{Id<GraphParameterId>(1), weight}}};
        const auto identity = Id<AnimationTickId>(tick);
        std::array<Math::Transform, 3> pose;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({identity, {10}, {}, parameters});
        const auto committed = instance.Commit(identity);
        const auto copied = instance.CopyPose(committed.publication.currentPose, pose);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        REQUIRE(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        REQUIRE(copied.HasValue());
        return pose;
    }
}  // namespace Horo::Animation::Test::PoseComposition
