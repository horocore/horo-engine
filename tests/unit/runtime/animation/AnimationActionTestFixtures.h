#pragma once
#include "AnimationGraphTestFixtures.h"
#include "Horo/Animation/AnimationGraphInstance.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <catch2/catch_message.hpp>
#include <memory>

namespace Horo::Animation::Test::Actions {
    /** @brief One real action sequence and named slot connected to the canonical base graph. */
    inline AnimationGraphData Data() {
        auto graph = Graph::Simple();
        GraphNode slot{Id<GraphNodeId>(5),
                       GraphActionSlotNode{Id<GraphActionSlotId>(1)},
                       {Graph::Pin(1, GraphPinRole::FirstPose, GraphValueType::Pose, false),
                        Graph::Pin(2, GraphPinRole::Result, GraphValueType::Pose, true)}};
        graph.definitions[0].nodes.push_back(slot);
        graph.definitions[0].connections = {Graph::Edge(4, 1, 5, 1), Graph::Edge(5, 2, 2, 1)};
        graph.actionSlots = {{Id<GraphActionSlotId>(1), "UpperBody"}};
        GraphActionDefinition action;
        action.id = Id<GraphActionId>(10);
        action.name = "Fire";
        action.slot = Id<GraphActionSlotId>(1);
        action.sequence.entry = Id<GraphActionSectionId>(1);
        action.sequence.sections = {{Id<GraphActionSectionId>(1), "Main", Asset<AnimationClipId>(4), {20}, {80}}};
        graph.actions.push_back(action);
        return graph;
    }

    /** @brief A second action reuses the actual base clip as an explicit sequence source. */
    inline void AddSecond(AnimationGraphData &graph) {
        auto action = graph.actions[0];
        action.id = Id<GraphActionId>(20);
        action.name = "Reload";
        action.sequence.sections[0].clip = Asset<AnimationClipId>(3);
        action.sequence.sections[0].begin = {0};
        action.sequence.sections[0].end = {100};
        graph.actions.push_back(action);
    }

    /** @brief Real immutable loop clips; region traversal supplies the authored one-shot policy without changing them. */
    inline AnimationClipAsset Clip(const SkeletonAsset &skeleton, const std::uint8_t id, const std::uint64_t generation) {
        AnimationClipData data;
        data.descriptor = {.id = Asset<AnimationClipId>(id),
                           .generation = Id<AnimationClipGeneration>(generation),
                           .skeleton = skeleton.Data().skeleton,
                           .skeletonGeneration = Id<SkeletonAssetGeneration>(5),
                           .duration = {100},
                           .wrapMode = AnimationWrapMode::Loop};
        Math::Transform first;
        first.translation.x = id == 3 ? 0.0F : 100.0F;
        auto last = first;
        last.translation.x += 100;
        data.tracks = {{Id<JointId>(1), {{{0}, first}, {{100}, last}}}};
        auto result = AnimationClipAsset::Create(std::move(data), skeleton);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Owns actual dependencies/compiler output; no fake action or pose plan is injected. */
    struct Prepared final {
        AnimationGraphData graph;
        std::shared_ptr<const SkeletonAsset> skeleton{std::make_shared<const SkeletonAsset>(Graph::GraphTestDependencies::MakeSkeleton())};
        std::vector<AnimationClipAsset> compileClips;
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        std::shared_ptr<const AnimationGraphProgram> program;
        GraphInstanceCreateContext context;

        explicit Prepared(AnimationGraphData data = Data(), const std::uint64_t generation = 6) : graph(std::move(data)) {
            compileClips.push_back(Clip(*skeleton, 3, generation));
            compileClips.push_back(Clip(*skeleton, 4, generation));
            for (const auto &clip : compileClips)
                clips.push_back(std::make_shared<const AnimationClipAsset>(clip));
            auto compiled = AnimationGraphProgram::Compile(graph, CompileContext());
            INFO((compiled.HasError() ? compiled.ErrorValue().message : "graph compiled"));
            REQUIRE(compiled.HasValue());
            program = std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value());
            context.instance = {Id<AnimationRuntimeId>(1), Id<AnimationComponentId>(2), {0, 1}};
        }

        [[nodiscard]] AnimationGraphCompileContext CompileContext() const {
            AnimationGraphCompileContext value;
            value.skeleton = skeleton.get();
            value.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
            value.clips = compileClips;
            return value;
        }

        [[nodiscard]] GraphInstanceAssets Assets() const {
            return {program, skeleton, Id<SkeletonAssetGeneration>(5), clips};
        }

        [[nodiscard]] AnimationGraphInstance Create() const {
            auto result = AnimationGraphInstance::Create(Assets(), context);
            INFO((result.HasError() ? result.ErrorValue().message : "instance created"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    };

    /** @brief Canonical typed play command. */
    inline GraphActionRequest Request(const std::uint64_t id = 10, const GraphActionQueuePolicy queue = GraphActionQueuePolicy::Queue) {
        return {Id<GraphActionId>(id), {}, queue, {}};
    }

    /** @brief Fixed caller-owned committed pose/slot/completion copies. */
    struct Tick final {
        GraphInstanceOutcome outcome;
        std::array<Math::Transform, 1> pose;
        std::array<GraphActionSlotObservation, 32> slots;
        std::array<GraphActionObservation, 256> completions;
        std::uint32_t slotCount{}, completionCount{};

        Tick(AnimationGraphInstance &instance, const std::uint64_t tick, const std::int64_t delta, const GraphPlaybackRate rate = {}) {
            const auto evaluated = instance.Evaluate({Id<AnimationTickId>(tick), {delta}, rate});
            INFO("graph failure=" << static_cast<unsigned>(evaluated.failure) << " clip=" << evaluated.clipFailure.detail);
            REQUIRE(evaluated.HasValue());
            outcome = instance.Commit(Id<AnimationTickId>(tick));
            REQUIRE(outcome.HasValue());
            REQUIRE(instance.CopyPose(outcome.publication.currentPose, pose).HasValue());
            REQUIRE(instance.CopyActionSlots(outcome.publication.currentPose, slots, slotCount).HasValue());
            REQUIRE(instance.CopyActionCompletions(outcome.publication.currentPose, completions, completionCount).HasValue());
        }
    };
}  // namespace Horo::Animation::Test::Actions
