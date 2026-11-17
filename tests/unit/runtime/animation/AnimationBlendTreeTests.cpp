#include "AnimationBlendTreeTestFixtures.h"

#include <catch2/catch_approx.hpp>
#include <limits>
#include <thread>

namespace Horo::Animation::Test {
    using namespace BlendTree;

    TEST_CASE("Animation 1D trees normalize real unequal-duration clip poses", "[animation][blend_tree]") {
        Prepared prepared;
        auto instance = prepared.Create();
        Tick first(instance, 1, 25);
        REQUIRE(first.count == 2);
        CHECK(first.members[0].weight == 0.5F);
        CHECK(first.members[1].weight == 0.5F);
        CHECK(first.members[0].sampleTime.ticks == 25);
        CHECK(first.members[1].sampleTime.ticks == 50);
        CHECK(first.pose[0].translation.x == 75.0F);
        Tick endpoint(instance, 2, 10, Coordinates(1));
        CHECK(endpoint.members[0].weight == 0.0F);
        CHECK(endpoint.members[1].weight == 1.0F);
        CHECK(endpoint.pose[0].translation.x == 130.0F);
        CHECK(endpoint.members[0].sampleTime.ticks == 25);
        CHECK(endpoint.members[1].leader);
    }

    TEST_CASE("Animation 1D tree hull policy is authored and rejected attempts leave last good cursors", "[animation][blend_tree]") {
        auto graph = Data();
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick low(instance, 1, 10, Coordinates(-100));
        CHECK(low.pose[0].translation.x == 10.0F);
        Tick high(instance, 2, 10, Coordinates(100));
        CHECK(high.members[1].weight == 1.0F);
        auto tree = Tree();
        tree.outside = GraphBlendOutside::Reject;
        graph = Data(tree);
        auto strict = prepared.Create(graph);
        Tick good(strict, 1, 10);
        CHECK(strict.Evaluate({Id<AnimationTickId>(2), {10}, {}, Coordinates(2)}).failure == GraphInstanceFailure::Malformed);
        CHECK(strict.Publication().currentPose == good.outcome.publication.currentPose);
        Tick retry(strict, 2, 10);
        CHECK(retry.members[0].sampleTime.ticks == 20);
    }

    TEST_CASE("Animation 2D trees use authored barycentric interiors and nearest hull edges", "[animation][blend_tree]") {
        const auto graph = Data(Triangle());
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick interior(instance, 1, 20, Coordinates(0.25F, 0.25F));
        REQUIRE(interior.count == 3);
        CHECK(interior.members[0].weight == 0.5F);
        CHECK(interior.members[1].weight == 0.25F);
        CHECK(interior.members[2].weight == 0.25F);
        CHECK(interior.pose[0].translation.x == 95.0F);
        Tick hull(instance, 2, 0, Coordinates(1, 1));
        CHECK(hull.members[0].weight == 0.0F);
        CHECK(hull.members[1].weight == 0.5F);
        CHECK(hull.members[2].weight == 0.5F);
        CHECK(hull.pose[0].translation.x == 170.0F);
    }

    TEST_CASE("Animation blend-space geometry rejects missing vertices degeneracy and overlapping faces", "[animation][blend_tree]") {
        const auto valid = Data(Triangle());
        Prepared prepared(valid);
        auto graph = valid;
        auto &tree = std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload);
        SECTION("unknown vertex") {
            tree.triangles[0].samples[2] = Id<GraphBlendSampleId>(999);
        }
        SECTION("repeated vertex") {
            tree.triangles[0].samples[2] = tree.triangles[0].samples[1];
        }
        SECTION("collinear") {
            tree.samples[2].x = 2;
            tree.samples[2].y = 0;
        }
        SECTION("duplicate face") {
            tree.triangles.push_back(tree.triangles[0]);
        }
        SECTION("unused sample") {
            tree.samples.push_back({Id<GraphBlendSampleId>(40), Asset<AnimationClipId>(3), 2, 2});
        }
        SECTION("nonfinite") {
            tree.samples[0].x = std::numeric_limits<float>::infinity();
        }
        CHECK(prepared.Compile(graph).HasError());
    }

    TEST_CASE("Animation tree parameter identity and type diagnostics remain exact", "[animation][blend_tree]") {
        Prepared prepared;
        auto graph = Data();
        SECTION("missing coordinate") {
            std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload).x = Id<GraphParameterId>(99);
        }
        SECTION("integer coordinate") {
            graph.parameters[0].type = GraphValueType::Integer;
            graph.parameters[0].defaultValue = std::int32_t{0};
        }
        SECTION("duplicate location") {
            std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload).samples[1].x = 0;
        }
        SECTION("empty samples") {
            std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload).samples.clear();
        }
        CHECK(prepared.Compile(graph).HasError());
    }

    TEST_CASE("Animation tree source round trips canonical sample identities and typed sync metadata", "[animation][blend_tree][source]") {
        auto graph = Data(Triangle());
        auto &tree = std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload);
        tree.group = Id<GraphSyncGroupId>(55);
        tree.sync = {GraphSyncMode::Markers, GraphSyncEventPolicy::AllActive};
        std::ranges::reverse(tree.samples);
        Prepared prepared(graph);
        auto encoded = SerializeAnimationGraphSource(graph, prepared.CompileContext());
        REQUIRE(encoded.HasValue());
        auto decoded = DeserializeAnimationGraphSource(encoded.Value(), graph.id, prepared.CompileContext());
        REQUIRE(decoded.HasValue());
        auto encodedAgain = SerializeAnimationGraphSource(decoded.Value(), prepared.CompileContext());
        REQUIRE(encodedAgain.HasValue());
        CHECK(encodedAgain.Value() == encoded.Value());
        REQUIRE(prepared.Compile(decoded.Value()).HasValue());
        graph.schemaVersion = 4;
        graph.contractVersion = {1, 2, 0};
        CHECK(MigrateAnimationGraph(graph).HasError());
    }

    TEST_CASE("Animation tree execution and publication copies allocate no frame memory", "[animation][blend_tree]") {
        Prepared prepared;
        auto instance = prepared.Create();
        std::array<GraphBlendMemberObservation, 2> members;
        std::array<Math::Transform, 1> pose;
        std::uint32_t written = 0;
        const auto tick = Id<AnimationTickId>(1);
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({tick, {25}});
        const auto committed = instance.Commit(tick);
        const auto copied = instance.CopyBlendMembers(committed.publication.currentPose, members, written);
        const auto poseCopied = instance.CopyPose(committed.publication.currentPose, pose);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(copied.HasValue());
        CHECK(poseCopied.HasValue());
        CHECK(written == 2);
    }

    TEST_CASE("Animation tree storage and work policies reject before frame execution", "[animation][blend_tree]") {
        Prepared prepared;
        auto compiled = prepared.Compile(Data());
        REQUIRE(compiled.HasValue());
        auto program = std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value());
        auto context = prepared.context;
        SECTION("storage") {
            context.maximumStorageBytes = 1;
        }
        SECTION("control") {
            context.maximumControlOperations = 1;
        }
        SECTION("joints") {
            context.maximumJointOperations = 1;
        }
        CHECK(AnimationGraphInstance::Create(prepared.Assets(program), context).HasError());
    }
}  // namespace Horo::Animation::Test
