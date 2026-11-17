#include "AnimationBlendTreeTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"

#include <catch2/catch_approx.hpp>
#include <cmath>
#include <limits>
#include <thread>

namespace Horo::Animation::Test {
    using namespace BlendTree;

    namespace {
        /** @brief Two tree occurrences feed the actual existing layer provider and share an explicit group. */
        AnimationGraphData SharedGroup(const GraphSyncEventPolicy policy = GraphSyncEventPolicy::LeaderOnly) {
            auto graph = Graph::Blended();
            graph.parameters.push_back({Id<GraphParameterId>(2), "y", GraphValueType::Float, 0.0F});
            auto first = Tree();
            first.samples.resize(1);
            first.group = Id<GraphSyncGroupId>(77);
            first.sync.events = policy;
            auto second = first;
            second.samples[0].clip = Asset<AnimationClipId>(4);
            graph.definitions[0].nodes[0].payload = first;
            graph.definitions[0].nodes[1].payload = second;
            return graph;
        }
    }  // namespace

    namespace {
        /** @brief Nested child trees share a group across source/destination lanes and are reentered through a distinct outer state. */
        AnimationGraphData NestedTrees() {
            auto graph = StateGraph::Data();
            auto child = StateGraph::Machine(graph);
            child.states[0].source = Id<GraphDefinitionId>(3);
            child.states[1].source = Id<GraphDefinitionId>(4);
            auto &outer = StateGraph::Machine(graph);
            outer.states[0].source = Id<GraphDefinitionId>(2);
            outer.states[2].source = Id<GraphDefinitionId>(2);
            outer.transitions[0].conditions = {{Id<GraphParameterId>(3), GraphStatePredicate::Equal, true}};
            graph.parameters.push_back({Id<GraphParameterId>(3), "outer", GraphValueType::Boolean, false});
            auto node = Graph::Clip(4);
            node.payload = child;
            graph.definitions.push_back({Id<GraphDefinitionId>(2), {}, {node, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
            auto tree = Tree();
            tree.samples.resize(1);
            tree.group = Id<GraphSyncGroupId>(77);
            node.payload = tree;
            graph.definitions.push_back({Id<GraphDefinitionId>(3), {}, {node, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
            tree.samples[0].clip = Asset<AnimationClipId>(4);
            node.payload = tree;
            graph.definitions.push_back({Id<GraphDefinitionId>(4), {}, {node, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
            return graph;
        }
    }  // namespace

    TEST_CASE("Animation sync groups align active players across independent tree occurrences", "[animation][sync]") {
        const auto graph = SharedGroup();
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick tick(instance, 1, 30);
        REQUIRE(tick.count == 2);
        CHECK(tick.members[0].group == tick.members[1].group);
        CHECK(tick.members[0].leader);
        CHECK_FALSE(tick.members[1].leader);
        CHECK(tick.members[0].sampleTime.ticks == 30);
        CHECK(tick.members[1].sampleTime.ticks == 60);
        CHECK(tick.members[0].eventsEligible);
        CHECK_FALSE(tick.members[1].eventsEligible);
        CHECK(tick.pose[0].translation.x == 80.0F);
    }

    TEST_CASE("Animation unnamed groups remain local and explicit follower eligibility adapts the actual leader rate",
              "[animation][sync]") {
        auto graph = SharedGroup();
        SECTION("local groups") {
            for (auto &node : graph.definitions[0].nodes)
                if (auto *tree = std::get_if<GraphBlendTreeNode>(&node.payload))
                    tree->group = {};
            Prepared prepared(graph);
            auto instance = prepared.Create(graph);
            Tick tick(instance, 1, 30);
            CHECK(tick.members[0].leader);
            CHECK(tick.members[1].leader);
            CHECK(tick.members[1].sampleTime.ticks == 30);
        }
        SECTION("authored follower") {
            std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload).samples[0].canLead = false;
            Prepared prepared(graph);
            auto instance = prepared.Create(graph);
            Tick tick(instance, 1, 30);
            CHECK_FALSE(tick.members[0].leader);
            CHECK(tick.members[1].leader);
            CHECK(tick.members[0].sampleTime.ticks == 15);
            CHECK(tick.members[1].sampleTime.ticks == 30);
        }
    }

    TEST_CASE("Animation sync markers adapt unequal authored intervals and report incompatible fallback", "[animation][sync]") {
        auto tree = Tree();
        tree.sync.mode = GraphSyncMode::Markers;
        const auto graph = Data(tree);
        Prepared prepared(graph, true);
        auto instance = prepared.Create(graph);
        Tick aligned(instance, 1, 25);
        CHECK(aligned.members[1].sampleTime.ticks == 75);
        CHECK_FALSE(aligned.members[1].markerFallback);
        CHECK(aligned.pose[0].translation.x == 81.25F);
        auto changed = prepared.compileClips[1].Data();
        changed.syncMarkers[1].id = Id<AnimationSyncMarkerId>(999);
        auto clip = AnimationClipAsset::Create(std::move(changed), *prepared.skeleton);
        REQUIRE(clip.HasValue());
        prepared.compileClips[1] = std::move(clip).Value();
        prepared.Refresh();
        auto fallback = prepared.Create(graph);
        Tick normalized(fallback, 1, 25);
        CHECK(normalized.members[1].sampleTime.ticks == 50);
        CHECK(normalized.members[1].markerFallback);
    }

    TEST_CASE("Animation sync traversal preserves reverse multi-loop bounds and residual rates", "[animation][sync]") {
        Prepared prepared;
        auto instance = prepared.Create();
        Tick wraps(instance, 1, 250);
        CHECK(wraps.members[0].sampleTime.ticks == 50);
        CHECK(wraps.members[1].sampleTime.ticks == 100);
        CHECK(wraps.members[0].boundaryCrossings == 2);
        CHECK(wraps.members[1].boundaryCrossings == 2);
        Tick reverse(instance, 2, 75, {}, GraphPlaybackRate{-1'000'000});
        CHECK(reverse.members[0].sampleTime.ticks == 75);
        CHECK(reverse.members[1].sampleTime.ticks == 150);
        CHECK(reverse.members[1].advance.ticks == -150);
        CHECK(reverse.members[1].direction == AnimationTraversalDirection::Reverse);
        CHECK(instance.Evaluate({Id<AnimationTickId>(3), {10'000}}).failure == GraphInstanceFailure::ClipFailure);
        Tick retry(instance, 3, 1);
        CHECK(retry.members[0].sampleTime.ticks == 76);
        auto tiny = prepared.Create();
        for (std::uint64_t i = 1; i <= 10; ++i) {
            Tick fractional(tiny, i, 1, {}, GraphPlaybackRate{100'000});
            CHECK(fractional.members[0].sampleTime.ticks == static_cast<std::int64_t>(i / 10));
        }
    }

    TEST_CASE("Animation sync event eligibility is an explicit all-active or suppressed filter", "[animation][sync]") {
        SECTION("all active") {
            const auto graph = SharedGroup(GraphSyncEventPolicy::AllActive);
            Prepared prepared(graph);
            auto instance = prepared.Create(graph);
            Tick tick(instance, 1, 10);
            CHECK(tick.members[0].eventsEligible);
            CHECK(tick.members[1].eventsEligible);
        }
        SECTION("suppressed") {
            const auto graph = SharedGroup(GraphSyncEventPolicy::Suppress);
            Prepared prepared(graph);
            auto instance = prepared.Create(graph);
            Tick tick(instance, 1, 10);
            CHECK_FALSE(tick.members[0].eventsEligible);
            CHECK_FALSE(tick.members[1].eventsEligible);
        }
    }

    TEST_CASE("Animation explicit sync policy conflicts fail cold admission", "[animation][sync]") {
        auto graph = SharedGroup();
        std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[1].payload).sync.mode = GraphSyncMode::Markers;
        Prepared prepared(graph);
        auto result = prepared.Compile(graph);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
    }

    TEST_CASE("Animation sync candidate abort cancelled commit reload and shutdown preserve last-good observations", "[animation][sync]") {
        Prepared prepared;
        auto instance = prepared.Create();
        Tick first(instance, 1, 10);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {40}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        Tick retry(instance, 2, 10);
        CHECK(retry.members[0].sampleTime.ticks == 20);
        CancellationSource cancellation;
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(3), {30}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(instance.Commit(Id<AnimationTickId>(3)).failure == GraphInstanceFailure::Cancelled);
        Tick after(instance, 3, 10);
        CHECK(after.members[0].sampleTime.ticks == 30);
        std::array<GraphBlendMemberObservation, 2> copied;
        std::uint32_t count = 0;
        CHECK(instance.CopyBlendMembers(first.outcome.publication.currentPose, copied, count).failure == GraphInstanceFailure::HandleStale);
        auto compiled = prepared.Compile(Data());
        REQUIRE(compiled.HasValue());
        auto assets = prepared.Assets(std::make_shared<const AnimationGraphProgram>(std::move(compiled).Value()));
        CHECK(instance.Replace(assets, prepared.context).HasError());
        auto replacement = prepared.context;
        ++replacement.instance.slot.generation;
        REQUIRE(instance.Replace(assets, replacement).HasValue());
        CHECK(instance.CopyBlendMembers(after.outcome.publication.currentPose, copied, count).failure == GraphInstanceFailure::HandleStale);
        REQUIRE(instance.Shutdown().HasValue());
        CHECK(instance.Evaluate({Id<AnimationTickId>(1), {1}}).failure == GraphInstanceFailure::AdmissionRejected);
    }

    TEST_CASE("Animation marker metadata rejects malformed immutable clip publications", "[animation][sync][clip]") {
        Prepared prepared;
        auto data = ClipData(3, true);
        SECTION("missing identity") {
            data.syncMarkers[0].id = {};
        }
        SECTION("duplicate times") {
            data.syncMarkers[1].time = data.syncMarkers[0].time;
        }
        SECTION("outside duration") {
            data.syncMarkers[1].time.ticks = 100;
        }
        SECTION("hard limit") {
            data.syncMarkers.resize(AnimationClipHardLimits::SyncMarkers + 1);
        }
        CHECK(AnimationClipAsset::Create(std::move(data), *prepared.skeleton).HasError());
    }

    TEST_CASE("Animation explicit groups join reusable definitions while preserving occurrence identity", "[animation][sync][calls]") {
        auto graph = SharedGroup();
        auto first = graph.definitions[0].nodes[0];
        auto second = graph.definitions[0].nodes[1];
        first.id = Id<GraphNodeId>(4);
        second.id = Id<GraphNodeId>(4);
        graph.definitions[0].nodes[0] = Graph::Call(4, 2);
        graph.definitions[0].nodes[1] = Graph::Call(5, 3);
        graph.definitions.push_back({Id<GraphDefinitionId>(2), {}, {first, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
        graph.definitions.push_back({Id<GraphDefinitionId>(3), {}, {second, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick tick(instance, 1, 30);
        REQUIRE(tick.count == 2);
        CHECK(tick.members[0].occurrence != tick.members[1].occurrence);
        CHECK(tick.members[0].sampleTime.ticks == 30);
        CHECK(tick.members[1].sampleTime.ticks == 60);
        CHECK(tick.members[0].leader != tick.members[1].leader);
    }

    TEST_CASE("Animation state transitions synchronize real source and newly entered tree players transactionally",
              "[animation][sync][state_graph]") {
        auto graph = StateGraph::Data();
        auto &machine = StateGraph::Machine(graph);
        machine.states[0].source = Id<GraphDefinitionId>(2);
        machine.states[1].source = Id<GraphDefinitionId>(3);
        auto first = Tree();
        first.samples.resize(1);
        first.group = Id<GraphSyncGroupId>(77);
        auto second = first;
        second.samples[0].clip = Asset<AnimationClipId>(4);
        auto node = Graph::Clip(4);
        node.payload = first;
        graph.definitions.push_back({Id<GraphDefinitionId>(2), {}, {node, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
        node.payload = second;
        graph.definitions.push_back({Id<GraphDefinitionId>(3), {}, {node, Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick entered(instance, 1, 10);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {10}, {}, speed}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        Tick transition(instance, 2, 10, speed);
        CHECK(transition.pose[0].translation.x == 20.0F);
        std::uint32_t active = 0, leaders = 0;
        for (std::size_t i = 0; i < transition.count; ++i) {
            const auto &member = transition.members[i];
            if (member.weight > 0) {
                ++active;
                leaders += member.leader;
            }
        }
        CHECK(active == 2);
        CHECK(leaders == 1);
        // Entry delta is held at zero by state control; synchronization still joins the leader's real interval.
        for (std::size_t i = 0; i < transition.count; ++i) {
            const auto &member = transition.members[i];
            if (member.weight <= 0.0F)
                continue;
            if (member.leader)
                CHECK(member.sampleTime.ticks == 20);
            else {
                CHECK(member.sampleTime.ticks == 40);
                CHECK(member.alignment.ticks == 20);
                CHECK_FALSE(member.eventsEligible);
            }
        }
        Tick blended(instance, 3, 25, speed);
        CHECK(blended.pose[0].translation.x == 70.0F);
        Tick dominant(instance, 4, 40, speed);
        CHECK(dominant.pose[0].translation.x == Catch::Approx(130.0F));
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(2)).HasValue());
        Tick interrupted(instance, 5, 25, speed);
        CHECK(interrupted.pose[0].translation.x == dominant.pose[0].translation.x);
    }

    TEST_CASE("Animation synchronized nonloop clips fail typed admission while independent ping-pong remains valid",
              "[animation][sync][clip]") {
        auto graph = Data();
        Prepared prepared(graph);
        auto data = prepared.compileClips[0].Data();
        data.descriptor.wrapMode = AnimationWrapMode::PingPong;
        auto clip = AnimationClipAsset::Create(std::move(data), *prepared.skeleton);
        REQUIRE(clip.HasValue());
        prepared.compileClips[0] = std::move(clip).Value();
        prepared.Refresh();
        auto incompatible = prepared.Compile(graph);
        REQUIRE(incompatible.HasError());
        CHECK(incompatible.ErrorValue().code.Value() == AnimationErrors::GraphTypeMismatch.code.Value());
        std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload).sync.mode = GraphSyncMode::Independent;
        auto independent = prepared.Create(graph);
        Tick reverseHalf(independent, 1, 150);
        CHECK(reverseHalf.members[0].sampleTime.ticks == 50);
        Tick held(independent, 2, 0);
        CHECK(held.members[0].sampleTime.ticks == 50);
    }

    TEST_CASE("Animation schema four state and pose graphs migrate without competing sync clocks", "[animation][sync][migration]") {
        auto graph = StateGraph::Data();
        graph.schemaVersion = 4;
        graph.contractVersion = {1, 2, 0};
        auto migrated = MigrateAnimationGraph(graph);
        REQUIRE(migrated.HasValue());
        CHECK(migrated.Value().schemaVersion == CurrentAnimationGraphSchemaVersion);
        CHECK(migrated.Value().contractVersion == CurrentAnimationGraphContractVersion);
        Instance::Prepared prepared(migrated.Value());
        auto instance = prepared.Create();
        StateGraph::Tick tick(instance, 1, 10);
        CHECK(tick.pose[0].translation.x == 10);
    }

    TEST_CASE("Animation marker intervals wrap around clip zero without a second clock", "[animation][sync][markers]") {
        auto tree = Tree();
        tree.sync.mode = GraphSyncMode::Markers;
        const auto graph = Data(tree);
        Prepared prepared(graph, true);
        for (std::size_t i = 0; i < prepared.compileClips.size(); ++i) {
            auto data = prepared.compileClips[i].Data();
            data.syncMarkers[0].time = {i == 0 ? 25 : 20};
            data.syncMarkers[1].time = {i == 0 ? 75 : 120};
            auto clip = AnimationClipAsset::Create(std::move(data), *prepared.skeleton);
            REQUIRE(clip.HasValue());
            prepared.compileClips[i] = std::move(clip).Value();
        }
        prepared.Refresh();
        auto instance = prepared.Create(graph);
        Tick joined(instance, 1, 0);
        CHECK(joined.members[1].sampleTime.ticks == 170);
        CHECK(joined.members[1].alignment.ticks == 170);
        Tick marker(instance, 2, 25);
        CHECK(marker.members[1].sampleTime.ticks == 20);
        CHECK(marker.members[1].alignment.ticks == 0);
        Tick loop(instance, 3, 100);
        CHECK(loop.members[1].sampleTime.ticks == 20);
        CHECK(loop.members[1].advance.ticks == 200);
    }

    TEST_CASE("Animation sync publication thread and capacity fences preserve caller-owned buffers", "[animation][sync]") {
        Prepared prepared;
        auto instance = prepared.Create();
        Tick tick(instance, 1, 10);
        std::array<GraphBlendMemberObservation, 2> output;
        output[0].weight = 42;
        std::uint32_t count = 99;
        CHECK(instance.CopyBlendMembers(tick.outcome.publication.currentPose, std::span{output}.first(1), count).failure ==
              GraphInstanceFailure::LimitExceeded);
        CHECK(count == 0);
        CHECK(output[0].weight == 42);
        GraphInstanceOutcome foreign;
        std::jthread worker([&] {
            std::uint32_t ownCount = 0;
            foreign = instance.CopyBlendMembers(tick.outcome.publication.currentPose, output, ownCount);
        });
        worker.join();
        CHECK(foreign.failure == GraphInstanceFailure::ThreadViolation);
        CHECK(output[0].weight == 42);
    }

    TEST_CASE("Animation tree preparation respects owner cancellation and hard sample limits", "[animation][sync]") {
        auto graph = Data();
        Prepared prepared(graph);
        auto context = prepared.CompileContext();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        context.cancellation = cancellation.Token();
        auto cancelled = AnimationGraphProgram::Compile(graph, context);
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == AnimationErrors::GraphOperationCancelled.code.Value());
        auto &tree = std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload);
        tree.samples.resize(AnimationGraphHardLimits::BlendSamplesPerTree + 1);
        auto oversized = prepared.Compile(graph);
        REQUIRE(oversized.HasError());
        CHECK(oversized.ErrorValue().code.Value() == AnimationErrors::GraphLimitExceeded.code.Value());
    }

    TEST_CASE("Animation nested sync tree branches retire and reenter through the canonical state transaction",
              "[animation][sync][state_graph]") {
        const auto graph = NestedTrees();
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick entered(instance, 1, 10);
        CHECK(entered.pose[0].translation.x == 10);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        Tick childTransition(instance, 2, 10, speed);
        Tick blended(instance, 3, 25, speed);
        CHECK(blended.pose[0].translation.x == 70);
        const std::array<GraphParameterInput, 2> leave{{{Id<GraphParameterId>(1), 2.0F}, {Id<GraphParameterId>(3), true}}};
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(4), {25}, {}, leave}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        Tick leaving(instance, 4, 25, leave);
        Tick left(instance, 5, 100, leave);
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(2)).HasValue());
        Tick reenter(instance, 6, 1);
        Tick reentered(instance, 7, 50);
        CHECK(reentered.pose[0].translation.x == 50);
    }

    TEST_CASE("Animation authored leader eligibility remains explicit across a zero-weight generic outer layer", "[animation][sync]") {
        auto graph = SharedGroup();
        graph.parameters[0].defaultValue = 0.0F;
        std::get<GraphBlendTreeNode>(graph.definitions[0].nodes[0].payload).samples[0].canLead = false;
        Prepared prepared(graph);
        auto instance = prepared.Create(graph);
        Tick tick(instance, 1, 30);
        CHECK_FALSE(tick.members[0].leader);
        CHECK(tick.members[1].leader);
        CHECK(tick.members[0].sampleTime.ticks == 15);
        CHECK(tick.members[1].sampleTime.ticks == 30);
        // Sampling computes (15/100)*100 in binary32; permit two ULPs at the exact mathematical result.
        constexpr float expected = 15.0F;
        const auto tolerance = 2.0F * (std::nextafter(expected, std::numeric_limits<float>::infinity()) - expected);
        CHECK(tick.pose[0].translation.x == Catch::Approx(expected).margin(tolerance));
    }

}  // namespace Horo::Animation::Test
