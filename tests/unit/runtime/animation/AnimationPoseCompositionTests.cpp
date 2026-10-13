#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <cmath>
#include <format>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Animation {
    namespace {
        using namespace Test;
        using namespace Test::Graph;

        /** @brief Creates real source graph operands for one explicit additive layer. */
        AnimationGraphData Layer(const GraphBlendMode mode = GraphBlendMode::Normalized) {
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
        AnimationGraphData AdditiveClipGraph() {
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
        AnimationGraphData RoundTrip(const AnimationGraphData &source) {
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
                return {std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value()), skeleton,
                        Id<SkeletonAssetGeneration>(5), clips};
            }

            AnimationGraphInstance Create() const {
                auto instance = AnimationGraphInstance::Create(Assets(), create);
                REQUIRE(instance.HasValue());
                return std::move(instance).Value();
            }
        };

        /** @brief Runs an actual fixed-tick transaction and checks its complete copied pose without hot allocation. */
        std::array<Math::Transform, 3> Evaluate(AnimationGraphInstance &instance, const float weight, const std::uint64_t tick = 1) {
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
    }  // namespace

    TEST_CASE("Graph dense masks use stable joints and explicit partial-pose completion", "[animation][graph][composition]") {
        auto source = Layer();
        auto &policy = std::get<GraphBlendNode>(source.definitions.front().nodes[3].payload);
        policy.mask = {0.0F, {{Id<JointId>(7), 1.0F}, {Id<JointId>(3), 0.5F}}};
        float missingTranslation = 0.0F;
        SECTION("legacy skeleton-reference completion") {
            policy.missingJoint = GraphMissingJointPolicy::SkeletonReference;
        }
        SECTION("preserve untracked base joints") {
            policy.missingJoint = GraphMissingJointPolicy::PreserveBase;
            missingTranslation = 10.0F;
        }
        LayerFixture fixture(source);
        auto instance = fixture.Create();
        const auto pose = Evaluate(instance, 1.0F);
        CHECK(pose[0].translation.x == 15.0F);
        CHECK(pose[1].translation.x == missingTranslation);
        CHECK(pose[2].translation.x == 10.0F);
    }

    TEST_CASE("Graph normalized weights clamp finite endpoints and use shortest-arc rotations", "[animation][graph][composition]") {
        LayerFixture fixture;
        auto instance = fixture.Create();
        CHECK(Evaluate(instance, -2.0F)[0].translation.x == 10.0F);
        CHECK(Evaluate(instance, 2.0F, 2)[0].translation.x == 20.0F);
        const auto midpoint = Evaluate(instance, 0.5F, 3);
        CHECK(midpoint[0].translation.x == 15.0F);
        CHECK(midpoint[0].scale.x == 3.0F);
        const auto &q = midpoint[0].rotation;
        CHECK(std::abs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0F) < 1.0e-5F);
    }

    TEST_CASE("Graph additive local and parent rotations have distinct noncommuting order", "[animation][graph][composition][additive]") {
        for (const auto mode : {GraphBlendMode::AdditiveLocal, GraphBlendMode::AdditiveParent}) {
            auto source = Layer(mode);
            std::get<GraphBlendNode>(source.definitions.front().nodes[3].payload).missingJoint = GraphMissingJointPolicy::PreserveBase;
            LayerFixture fixture(source);
            auto instance = fixture.Create();
            const auto pose = Evaluate(instance, 1.0F);
            CHECK(pose[0].translation.x == 25.0F);
            CHECK(pose[0].scale.x == 8.0F);
            const auto axis = pose[0].rotation.Rotate({0, 0, 1});
            if (mode == GraphBlendMode::AdditiveLocal) {
                CHECK(std::abs(axis.x - 1.0F) < 1.0e-5F);
                CHECK(std::abs(axis.y) < 1.0e-5F);
            } else {
                CHECK(std::abs(axis.x) < 1.0e-5F);
                CHECK(std::abs(axis.y + 1.0F) < 1.0e-5F);
            }
            CHECK(pose[1].translation.x == 10.0F);
        }
    }

    TEST_CASE("Graph malformed mask modes joints and reference operands reject during real compilation",
              "[animation][graph][composition]") {
        auto source = Layer();
        auto &policy = std::get<GraphBlendNode>(source.definitions.front().nodes[3].payload);
        SECTION("unknown joint") {
            policy.mask.joints = {{Id<JointId>(99), 0.5F}};
        }
        SECTION("duplicate joint") {
            policy.mask.joints = {{Id<JointId>(3), 0.5F}, {Id<JointId>(3), 0.7F}};
        }
        SECTION("nonfinite weight") {
            policy.mask.defaultWeight = std::numeric_limits<float>::infinity();
        }
        SECTION("negative weight") {
            policy.mask.defaultWeight = -0.1F;
        }
        SECTION("unsupported mode") {
            policy.mode = GraphBlendMode::Count;
        }
        SECTION("unsupported missing policy") {
            policy.missingJoint = GraphMissingJointPolicy::Count;
        }
        SECTION("missing additive reference") {
            policy.mode = GraphBlendMode::AdditiveLocal;
        }
        LayerFixture fixture(source);
        CHECK(fixture.Compile().HasError());
    }

    TEST_CASE("Graph dense composition storage work cancellation and reload stay transactional",
              "[animation][graph][composition][lifecycle]") {
        LayerFixture fixture(Layer(GraphBlendMode::AdditiveLocal));
        auto instance = fixture.Create();
        const auto pose = Evaluate(instance, 0.5F);
        const auto original = instance.Publication();
        auto assets = fixture.Assets();
        auto replacement = fixture.create;
        replacement.instance.slot.generation = 2;
        replacement.maximumStorageBytes = 1;
        CHECK(instance.Replace(assets, replacement).HasError());
        CHECK(instance.Publication().currentPose == original.currentPose);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(instance.Evaluate({Id<AnimationTickId>(2), {10}, {}, {}, cancelled.Token()}).failure == GraphInstanceFailure::Cancelled);
        CHECK(instance.Publication().currentPose == original.currentPose);
        auto tight = fixture.create;
        tight.maximumJointOperations = 1;
        CHECK(AnimationGraphInstance::Create(assets, tight).HasError());
        CHECK(instance.Shutdown().HasValue());
        CHECK(instance.Shutdown().HasValue());
        CHECK(instance.Evaluate({Id<AnimationTickId>(3), {10}}).HasError());
        std::array<Math::Transform, 3> retained;
        CHECK(instance.CopyPose(original.currentPose, retained).HasValue());
        CHECK(retained == pose);
    }

    TEST_CASE("Graph additive layers cancel at commit and retire old handles only on successful reload",
              "[animation][graph][composition][lifecycle]") {
        LayerFixture fixture(Layer(GraphBlendMode::AdditiveParent));
        const auto assets = fixture.Assets();
        auto instance = fixture.Create();
        const auto originalPose = Evaluate(instance, 0.5F);
        const auto original = instance.Publication();
        CancellationSource cancellation;
        const auto tick = Id<AnimationTickId>(2);
        REQUIRE(instance.Evaluate({tick, {10}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(instance.Commit(tick).failure == GraphInstanceFailure::Cancelled);
        CHECK(instance.Publication().currentPose == original.currentPose);
        std::array<Math::Transform, 3> retained;
        CHECK(instance.CopyPose(original.currentPose, retained).HasValue());
        CHECK(retained == originalPose);
        auto replacement = fixture.create;
        replacement.instance.slot.generation = 2;
        REQUIRE(instance.Replace(assets, replacement).HasValue());
        CHECK(instance.CopyPose(original.currentPose, retained).failure == GraphInstanceFailure::HandleStale);
        CHECK(Evaluate(instance, 0.5F) == originalPose);
    }

    TEST_CASE("Graph schema two migration preserves stable IDs and normalized defaults", "[animation][graph][composition][codec]") {
        const auto source = Layer();
        const auto encoded = SerializeAnimationGraphSource(source);
        REQUIRE(encoded.HasValue());
        auto old = nlohmann::ordered_json::parse(encoded.Value());
        old["schemaVersion"] = 2;
        old["contractVersion"]["minor"] = 0;
        for (auto &node : old["payload"]["definitions"][0]["nodes"])
            node.erase("settings");
        CHECK(DeserializeAnimationGraphSource(old.dump(), source.id).HasError());
        CHECK(DeserializeAnimationGraphSource(old.dump(), source.id, {}, {}, AnimationGraphSourceMigration::MigrateVersion1).HasError());
        const auto migrated =
            DeserializeAnimationGraphSource(old.dump(), source.id, {}, {}, AnimationGraphSourceMigration::MigrateLegacySchemas);
        REQUIRE(migrated.HasValue());
        const auto rewritten = SerializeAnimationGraphSource(migrated.Value());
        REQUIRE(rewritten.HasValue());
        CHECK(rewritten.Value() == encoded.Value());
        LayerFixture fixture(migrated.Value());
        CHECK(fixture.Compile().HasValue());
    }

    TEST_CASE("Graph layers follow data dependency order rather than node vector order", "[animation][graph][composition]") {
        auto source = Layer();
        auto &definition = source.definitions.front();
        auto overlay = Clip(7);
        std::get<GraphClipNode>(overlay.payload).clip = Asset<AnimationClipId>(5);
        definition.nodes.push_back(std::move(overlay));
        definition.nodes.push_back(Blend(8));
        definition.connections.pop_back();
        definition.connections.insert(definition.connections.end(),
                                      {Edge(6, 4, 8, 1), Edge(7, 1, 8, 2), Edge(3, 1, 8, 3, GraphValueType::Float), Edge(8, 4, 2, 1)});
        std::ranges::reverse(definition.nodes);
        LayerFixture fixture(source);
        auto instance = fixture.Create();
        CHECK(Evaluate(instance, 0.5F)[0].translation.x == 10.0F);
    }

    TEST_CASE("Graph additive clip binds an explicit reference identity and complete base and reference operands",
              "[animation][graph][composition][additive]") {
        const auto source = AdditiveClipGraph();
        CHECK(RoundTrip(source).id == source.id);
        LayerFixture fixture(source, true);
        SECTION("actual pinned additive sampling and missing track preservation") {
            auto instance = fixture.Create();
            const auto tick = Id<AnimationTickId>(1);
            std::array<Math::Transform, 3> pose;
            const auto before = HoroAnimationTestAllocationCount();
            const auto evaluated = instance.Evaluate({tick, {10}});
            const auto committed = instance.Commit(tick);
            const auto copied = instance.CopyPose(committed.publication.currentPose, pose);
            CHECK(HoroAnimationTestAllocationCount() == before);
            REQUIRE(evaluated.HasValue());
            REQUIRE(committed.HasValue());
            REQUIRE(copied.HasValue());
            CHECK(pose[0].translation.x == 25.0F);
            CHECK(pose[0].scale.x == 8.0F);
            CHECK(pose[1].translation.x == 10.0F);
            CHECK(pose[2].translation.x == 10.0F);
        }
        SECTION("wrong authored reference identity") {
            std::get<GraphClipNode>(fixture.graph.definitions.front().nodes[0].payload).referencePose = Id<AnimationReferencePoseId>(45);
            CHECK(fixture.Compile().HasError());
        }
        SECTION("retired reference generation in runtime dependency") {
            auto assets = fixture.Assets();
            auto changed = fixture.compileClips[1].Data();
            REQUIRE(changed.descriptor.kind == AnimationClipKind::Additive);
            changed.descriptor.referencePose->generation = Id<AnimationReferencePoseGeneration>(10);
            auto stale = AnimationClipAsset::Create(std::move(changed), *fixture.skeleton);
            REQUIRE(stale.HasValue());
            fixture.clips[1] = std::make_shared<const AnimationClipAsset>(std::move(stale).Value());
            CHECK(AnimationGraphInstance::Create(assets, fixture.create).HasError());
        }
        SECTION("absolute clip contradicts additive declaration") {
            LayerFixture absolute(source);
            CHECK(absolute.Compile().HasError());
        }
    }

    TEST_CASE("Graph composition singular candidates do not publish and a later valid attempt recovers",
              "[animation][graph][composition][lifecycle]") {
        LayerFixture fixture;
        auto changed = fixture.compileClips[1].Data();
        for (auto &track : changed.tracks)
            for (auto &key : track.keys)
                key.transform.scale = {-2, -2, -2};
        auto clip = AnimationClipAsset::Create(std::move(changed), *fixture.skeleton);
        REQUIRE(clip.HasValue());
        fixture.compileClips[1] = std::move(clip).Value();
        fixture.RefreshClips();
        auto instance = fixture.Create();
        const auto old = Evaluate(instance, 0.0F);
        const auto publication = instance.Publication();
        const std::array<GraphParameterInput, 1> half{{{Id<GraphParameterId>(1), 0.5F}}};
        const auto before = HoroAnimationTestAllocationCount();
        const auto failure = instance.Evaluate({Id<AnimationTickId>(2), {10}, {}, half});
        CHECK(HoroAnimationTestAllocationCount() == before);
        CHECK(failure.failure == GraphInstanceFailure::Malformed);
        CHECK(instance.Publication().currentPose == publication.currentPose);
        std::array<Math::Transform, 3> retained;
        CHECK(instance.CopyPose(publication.currentPose, retained).HasValue());
        CHECK(retained == old);
        CHECK(Evaluate(instance, 0.0F, 3) == old);
    }

    TEST_CASE("Graph layered source settings round trip canonically and reject malformed shapes",
              "[animation][graph][composition][codec]") {
        auto source = Layer(GraphBlendMode::AdditiveParent);
        auto &policy = std::get<GraphBlendNode>(source.definitions.front().nodes[3].payload);
        policy.mask = {0.0F, {{Id<JointId>(11), 0.75F}, {Id<JointId>(3), 0.5F}}};
        policy.missingJoint = GraphMissingJointPolicy::PreserveBase;
        const auto encoded = SerializeAnimationGraphSource(source);
        REQUIRE(encoded.HasValue());
        LayerFixture fixture(RoundTrip(source));
        CHECK(fixture.Compile().HasValue());
        auto json = nlohmann::ordered_json::parse(encoded.Value());
        auto node = std::ranges::find_if(json["payload"]["definitions"][0]["nodes"], [](const auto &value) {
            return value["kind"] == "blend";
        });
        REQUIRE(node != json["payload"]["definitions"][0]["nodes"].end());
        SECTION("unknown mode") {
            (*node)["settings"]["mode"] = "unknown";
        }
        SECTION("negative mask") {
            (*node)["settings"]["mask"]["defaultWeight"] = -1;
        }
        SECTION("missing required setting") {
            (*node)["settings"].erase("missingJoint");
        }
        SECTION("unexpected setting") {
            (*node)["settings"]["extra"] = true;
        }
        CHECK(DeserializeAnimationGraphSource(json.dump(), source.id).HasError());
    }
}  // namespace Horo::Animation
