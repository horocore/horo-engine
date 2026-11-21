#pragma once

#include "../../../unit/runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "AnimationActionTestFixtures.h"
#include "AnimationGraphInstanceTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"
#include "Horo/Animation/AnimationCookedAssets.h"
#include "Horo/Assets/AssetCook.h"

namespace Horo::Network::NativeQualification::LiveScene {
    enum class GraphFixture {
        Velocity,
        ActionAuthority,
        ActionReplica
    };

    /** @brief Drives a visible joint pose from committed replication through real cooked assets. */
    class PresentationAnimation final {
        using Payload = Animation::AnimationCookedPayload;
        using Prepared = Animation::Test::Instance::Prepared;

        template <class T> static T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        static Animation::AnimationGraphData Graph(GraphFixture selected) {
            using namespace Animation;
            using Test::Id;
            if (selected == GraphFixture::ActionAuthority) {
                auto graph = Test::Actions::Data();
                auto &section = graph.actions.front().sequence.sections.front();
                section.begin = {0};
                section.end = {AnimationTicksPerSecond * 3 / 4};
                return graph;
            }
            if (selected == GraphFixture::ActionReplica) {
                auto graph = Test::StateGraph::Data();
                graph.parameters = {{Parameter(), "action", GraphValueType::Integer, std::int32_t{0}}};
                auto &machine = Test::StateGraph::Machine(graph);
                machine.states.resize(2);
                machine.transitions = {{.id = Id<GraphTransitionId>(100),
                                        .source = Id<GraphStateId>(10),
                                        .destination = Id<GraphStateId>(20),
                                        .conditions = {{Parameter(), GraphStatePredicate::Equal, PublicTag}}},
                                       {.id = Id<GraphTransitionId>(200),
                                        .source = Id<GraphStateId>(20),
                                        .destination = Id<GraphStateId>(10),
                                        .conditions = {{Parameter(), GraphStatePredicate::Equal, std::int32_t{0}}}}};
                return graph;
            }
            auto graph = Animation::Test::Instance::Blended();
            graph.parameters.front().defaultValue = 0.0F;
            return graph;
        }

    public:
        static constexpr std::int32_t PublicTag = 31;

        static Animation::GraphParameterId Parameter() {
            return Animation::Test::Id<Animation::GraphParameterId>(1);
        }

        /** @brief Installs the selected graph and its real committed pose assets. */
        static Animation::AnimationAuthoringComponent Install(Assets::AssetRegistry &registry, Assets::MemoryAssetProvider &provider,
                                                              AssetCookTargetId target, GraphFixture selected) {
            const Prepared fixture{Animation::Test::Instance::Blended()};
            auto skeleton = Take(Animation::EncodeSkeletonAnimationPayload(*fixture.skeleton));
            std::vector<Animation::AnimationClipAsset> constantClips;
            std::vector<Payload> clips;
            for (const auto &compiled : fixture.compileClips) {
                auto data = compiled.Data();
                data.descriptor.duration = {Animation::AnimationTicksPerSecond};
                for (auto &track : data.tracks) {
                    track.keys.back().time = {data.descriptor.duration.ticks};
                    track.keys.back().transform = track.keys.front().transform;
                }
                constantClips.push_back(Take(Animation::AnimationClipAsset::Create(std::move(data), *fixture.skeleton)));
                clips.push_back(Take(Animation::EncodeClipAnimationPayload(constantClips.back(), skeleton.bytes)));
            }
            const auto skeletonType = Take(Assets::AssetTypeId::Parse("core.animation.skeleton"));
            const auto clipType = Take(Assets::AssetTypeId::Parse("core.animation.clip"));
            std::vector<Animation::AnimationCookedDependencyInput> inputs{{{skeleton.asset, skeletonType}, skeleton.bytes}};
            for (const auto &clip : clips)
                inputs.push_back({{clip.asset, clipType}, clip.bytes});
            std::ranges::sort(inputs, {}, [](const auto &input) {
                return input.metadata.id;
            });
            const auto program =
                Take(Animation::AnimationGraphProgram::Compile(Graph(selected), {.skeleton = fixture.skeleton.get(),
                                                                                 .skeletonGeneration = fixture.Assets().skeletonGeneration,
                                                                                 .clips = constantClips}));
            auto graph = Take(Animation::EncodeGraphAnimationPayload(program, inputs));
            std::vector<Assets::AssetRecord> records;
            const auto add = [&](Payload payload, Assets::AssetTypeId type) {
                const auto digest = ComputeSha256(std::as_bytes(std::span{payload.bytes}));
                const auto artifact =
                    Take(Assets::EncodeCookedArtifact({payload.asset, type, target, digest, digest, digest, std::move(payload.bytes)}));
                records.push_back(Physics::Record(payload.asset, type));
                provider.Insert(payload.asset, artifact);
            };
            add(std::move(skeleton), skeletonType);
            for (auto &clip : clips)
                add(std::move(clip), clipType);
            add(std::move(graph), Take(Assets::AssetTypeId::Parse("core.animation.graph")));
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            Animation::AnimationAuthoringComponent binding;
            binding.component = Animation::Test::Id<Animation::AnimationComponentId>(1);
            binding.skeleton = fixture.skeleton->Data().skeleton;
            binding.source.kind = Animation::AnimationSourceKind::Graph;
            binding.source.graph = program.Id();
            return binding;
        }
    };
}  // namespace Horo::Network::NativeQualification::LiveScene
