#include "AnimationStateGraphTestFixtures.h"

#include <limits>
#include <thread>

namespace Horo::Animation::Test {
    using Instance::Prepared;
    using StateGraph::Tick;

    TEST_CASE("Animation compiled state nodes advance only active players and atomically commit transition poses",
              "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        REQUIRE(fixture.program->StateMachines().size() == 1);
        REQUIRE(fixture.program->Memory().clipPlayers == 6);
        auto instance = fixture.Create();
        Tick initial(instance, 1, 10);
        CHECK(initial.pose[0].translation.x == 10.0F);
        REQUIRE(initial.count == 1);
        CHECK(initial.events[0].kind == GraphStateEventKind::Entered);
        CHECK(initial.events[0].state == Id<GraphStateId>(10));
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        Tick started(instance, 2, 10, speed);
        CHECK(started.pose[0].translation.x == 20.0F);
        REQUIRE(started.count == 2);
        CHECK(started.events[1].state == Id<GraphStateId>(20));
        Tick blended(instance, 3, 25, speed);
        CHECK(blended.pose[0].translation.x == 65.0F);
        CHECK(blended.count == 0);
        Tick completed(instance, 4, 75, speed);
        CHECK(completed.pose[0].translation.x == 100.0F);
        REQUIRE(completed.count == 4);
        CHECK(completed.events[0].kind == GraphStateEventKind::Loop);
        CHECK(completed.events[1].kind == GraphStateEventKind::Loop);
        CHECK(completed.events[2].kind == GraphStateEventKind::Exited);
        CHECK(completed.events[3].kind == GraphStateEventKind::TransitionCompleted);
    }

    TEST_CASE("Animation compiled state interruption reuses committed composition and survives aborted attempts",
              "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        auto instance = fixture.Create();
        Tick initial(instance, 1, 10);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        Tick started(instance, 2, 10, speed);
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(2)).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(3), {25}, {}, speed}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK(instance.Publication().tick == Id<AnimationTickId>(2));
        Tick interrupted(instance, 3, 25, speed);
        CHECK(interrupted.pose[0].translation.x == started.pose[0].translation.x);
        REQUIRE(interrupted.count == 4);
        CHECK(interrupted.events[0].kind == GraphStateEventKind::TransitionInterrupted);
        CHECK(interrupted.events[3].activation == 3);
        Tick continued(instance, 4, 25, speed);
        CHECK(continued.pose[0].translation.x == 22.5F);
        Tick completed(instance, 5, 25, speed);
        CHECK(completed.pose[0].translation.x == 50.0F);
        REQUIRE(completed.count != 0);
        CHECK(completed.events[completed.count - 1].kind == GraphStateEventKind::TransitionCompleted);
    }

    TEST_CASE("Animation compiled state cut self transitions restart the exact entered lane", "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        auto &transition = StateGraph::Machine(graph).transitions[0];
        transition.destination = transition.source;
        transition.duration = {0};
        Prepared fixture(graph);
        auto instance = fixture.Create();
        Tick initial(instance, 1, 10);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        Tick reentered(instance, 2, 10, speed);
        CHECK(reentered.pose[0].translation.x == 0.0F);
        REQUIRE(reentered.count == 4);
        CHECK(reentered.events[1].activation == 2);
        CHECK(reentered.events[2].activation == 1);
        Tick next(instance, 3, 10);
        CHECK(next.pose[0].translation.x == 10.0F);
        CHECK(next.count == 0);
    }

    TEST_CASE("Animation compiled state pose parameters and event copies perform no dynamic allocation", "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        std::array<Math::Transform, 1> pose;
        std::array<GraphParameterInput, 2> parameters;
        std::array<GraphStateEvent, 262> events;
        std::uint32_t count = 0;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({tick, {10}});
        const auto committed = instance.Commit(tick);
        const auto poseCopied = instance.CopyPose(committed.publication.currentPose, pose);
        const auto parametersCopied = instance.CopyParameters(committed.publication.currentPose, parameters);
        const auto eventsCopied = instance.CopyStateEvents(committed.publication.currentPose, events, count);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(poseCopied.HasValue());
        CHECK(parametersCopied.HasValue());
        CHECK(eventsCopied.HasValue());
        CHECK(count == 1);
        CHECK(events[0].instance == fixture.context.instance);
        CHECK(events[0].tick == tick);
        CHECK(events[0].source.node == fixture.program->Occurrences()[events[0].occurrence].source.node);
    }

    TEST_CASE("Animation compiled state event publications retain owner generation and shutdown fences", "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        auto instance = fixture.Create();
        Tick first(instance, 1, 10);
        std::array<GraphStateEvent, 262> output;
        std::uint32_t count = 99;
        CHECK(instance.CopyStateEvents(first.outcome.publication.currentPose, std::span{output}.first(0), count).failure ==
              GraphInstanceFailure::LimitExceeded);
        CHECK(count == 99);
        Tick second(instance, 2, 10);
        REQUIRE(instance.CopyStateEvents(first.outcome.publication.currentPose, output, count).HasValue());
        CHECK(count == 1);
        Tick third(instance, 3, 10);
        CHECK(instance.CopyStateEvents(first.outcome.publication.currentPose, output, count).failure == GraphInstanceFailure::HandleStale);
        GraphInstanceOutcome foreign;
        std::jthread thread([&] {
            foreign = instance.CopyStateEvents(third.outcome.publication.currentPose, output, count);
        });
        thread.join();
        CHECK(foreign.failure == GraphInstanceFailure::ThreadViolation);
        REQUIRE(instance.Shutdown().HasValue());
        REQUIRE(instance.Shutdown().HasValue());
        REQUIRE(instance.CopyStateEvents(third.outcome.publication.currentPose, output, count).HasValue());
        CHECK(instance.Evaluate({Id<AnimationTickId>(4), {10}}).failure == GraphInstanceFailure::AdmissionRejected);
    }

    TEST_CASE("Animation compiled state cancelled ticks and failed reload retain queued triggers and publications",
              "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        auto instance = fixture.Create();
        Tick first(instance, 1, 10);
        CancellationSource cancellation;
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(2)).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {10}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(instance.Commit(Id<AnimationTickId>(2)).failure == GraphInstanceFailure::Cancelled);
        CHECK(instance.Publication().currentPose == first.outcome.publication.currentPose);
        auto invalid = fixture.Assets();
        invalid.skeletonGeneration = Id<SkeletonAssetGeneration>(99);
        const auto replacement = fixture.ReplacementContext();
        CHECK(instance.Replace(invalid, replacement).HasError());
        Tick retry(instance, 2, 10);
        std::array<GraphParameterInput, 2> parameters;
        REQUIRE(instance.CopyParameters(retry.outcome.publication.currentPose, parameters).HasValue());
        CHECK(std::get<bool>(parameters[1].value));
        REQUIRE(instance.Replace(fixture.Assets(), replacement).HasValue());
        std::array<GraphStateEvent, 262> events;
        std::uint32_t count = 0;
        CHECK(instance.CopyStateEvents(retry.outcome.publication.currentPose, events, count).failure == GraphInstanceFailure::HandleStale);
        Tick restarted(instance, 1, 10);
        CHECK(restarted.events[0].activation == 1);
    }

    TEST_CASE("Animation compiled state playback holds reverses and rejects complete over-budget attempts", "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        // Isolate the state-event ceiling while retaining the clip's immutable hard traversal ceiling.
        fixture.context.clipLimits.maximumBoundaryCrossings = AnimationClipHardLimits::BoundaryCrossings;
        auto instance = fixture.Create();
        Tick initial(instance, 1, 10);
        Tick held(instance, 2, 90, {}, {0});
        CHECK(held.pose[0].translation.x == initial.pose[0].translation.x);
        CHECK(held.count == 0);
        Tick reverse(instance, 3, 20, {}, {-1'000'000});
        CHECK(reverse.pose[0].translation.x == 90.0F);
        REQUIRE(reverse.count == 1);
        CHECK(reverse.events[0].direction == AnimationTraversalDirection::Reverse);
        CHECK(instance.Evaluate({Id<AnimationTickId>(4), {30'000}}).failure == GraphInstanceFailure::LimitExceeded);
        CHECK(instance.Publication().tick == Id<AnimationTickId>(3));
        Tick retry(instance, 4, 10);
        CHECK(retry.pose[0].translation.x == 0.0F);
    }

    TEST_CASE("Animation compiled state subgraphs execute through actual call access plans and reset on entry",
              "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        StateGraph::Machine(graph).states[0].source = Id<GraphDefinitionId>(2);
        StateGraph::Machine(graph).states[0].loopPeriod = AnimationDeltaTime{100};
        graph.definitions.push_back({Id<GraphDefinitionId>(2), {}, {Graph::Clip(4), Graph::Output(2)}, {Graph::Edge(4, 1, 2, 1)}});
        Prepared fixture(graph);
        auto instance = fixture.Create();
        Tick initial(instance, 1, 25);
        CHECK(initial.pose[0].translation.x == 25.0F);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        Tick started(instance, 2, 25, speed);
        CHECK(started.pose[0].translation.x == 50.0F);
        Tick advanced(instance, 3, 25, speed);
        CHECK(advanced.pose[0].translation.x == 87.5F);
    }

    TEST_CASE("Animation nested state reentry never reuses the earlier occurrence activation", "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        const auto child = StateGraph::Machine(graph);
        auto &parent = StateGraph::Machine(graph);
        parent.states = {{Id<GraphStateId>(10), Id<GraphDefinitionId>(2)}};
        parent.transitions = {{Id<GraphTransitionId>(1000),
                               Id<GraphStateId>(10),
                               Id<GraphStateId>(10),
                               0,
                               {0},
                               {},
                               GraphStateInterruption::None,
                               GraphStateBlendCurve::Linear,
                               {{Id<GraphParameterId>(2), GraphStatePredicate::Trigger, true}}}};
        StateGraph::NestedFixture fixture{std::move(graph), child};
        auto &instance = fixture.instance;
        const auto &first = fixture.entered;
        const auto childOccurrence = first.events[1].occurrence;
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(2)).HasValue());
        Tick second(instance, 2, 10);
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(2)).HasValue());
        Tick third(instance, 3, 10);
        bool found = false;
        for (const auto &event : std::span{third.events}.first(third.count))
            if (event.kind == GraphStateEventKind::Entered && event.occurrence == childOccurrence) {
                found = true;
                CHECK(event.activation > first.events[1].activation);
            }
        CHECK(found);
        bool exited = false;
        for (const auto &event : std::span{second.events}.first(second.count))
            if (event.kind == GraphStateEventKind::Exited && event.occurrence == childOccurrence) {
                exited = true;
                CHECK(event.activation == first.events[1].activation);
            }
        CHECK(exited);
    }

    TEST_CASE("Animation state cut retires the detached old subtree after candidate runtime changes", "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        const auto child = StateGraph::Machine(graph);
        StateGraph::Machine(graph).states[0].source = Id<GraphDefinitionId>(2);
        StateGraph::Machine(graph).transitions[0].duration = {0};
        StateGraph::NestedFixture fixture{std::move(graph), child};
        auto &instance = fixture.instance;
        const auto &first = fixture.entered;
        const auto oldChild = first.events[1];
        REQUIRE(oldChild.kind == GraphStateEventKind::Entered);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        Tick cut(instance, 2, 10, speed);
        std::size_t childExits = 0;
        for (const auto &event : std::span{cut.events}.first(cut.count)) {
            if (event.occurrence != oldChild.occurrence)
                continue;
            CHECK(event.kind == GraphStateEventKind::Exited);
            CHECK(event.state == oldChild.state);
            CHECK(event.activation == oldChild.activation);
            ++childExits;
        }
        CHECK(childExits == 1);
        CHECK(cut.pose[0].translation.x == 100.0F);
        Tick next(instance, 3, 10);
        CHECK(next.pose[0].translation.x == 110.0F);
        for (const auto &event : std::span{next.events}.first(next.count))
            CHECK(event.occurrence != oldChild.occurrence);
    }

    TEST_CASE("Animation state graph exact exit-time gates retain hysteresis and include initial traversal", "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        StateGraph::Machine(graph).transitions[0].exitTime = AnimationTime{100};
        StateGraph::Machine(graph).transitions[0].duration = {0};
        Prepared fixture(graph);
        auto instance = fixture.Create();
        const std::array<GraphParameterInput, 1> enter{{{Id<GraphParameterId>(1), 2.0F}}};
        const std::array<GraphParameterInput, 1> hold{{{Id<GraphParameterId>(1), 0.75F}}};
        Tick first(instance, 1, 25, enter);
        CHECK(first.pose[0].translation.x == 25.0F);
        REQUIRE(first.count == 1);
        Tick before(instance, 2, 50, hold);
        CHECK(before.count == 0);
        Tick boundary(instance, 3, 25, hold);
        CHECK(boundary.pose[0].translation.x == 100.0F);
        REQUIRE(boundary.count == 5);
        CHECK(boundary.events[0].kind == GraphStateEventKind::Loop);
        CHECK(boundary.events[4].kind == GraphStateEventKind::TransitionCompleted);

        auto second = fixture.Create();
        Tick initialBoundary(second, 1, 100, enter);
        CHECK(initialBoundary.pose[0].translation.x == 100.0F);
        REQUIRE(initialBoundary.count == 6);
        CHECK(initialBoundary.events[0].kind == GraphStateEventKind::Entered);
        CHECK(initialBoundary.events[1].kind == GraphStateEventKind::Loop);
        CHECK(initialBoundary.events[5].kind == GraphStateEventKind::TransitionCompleted);
    }

    TEST_CASE("Animation state graph preparation charges complete state control and event storage", "[animation][state_graph]") {
        Prepared fixture(StateGraph::Data());
        auto context = fixture.context;
        context.maximumControlOperations = 0;
        CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasError());
        context = fixture.context;
        context.maximumStorageBytes = 1024;
        CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasError());
        auto instance = fixture.Create();
        Tick initial(instance, 1, 10);
        const auto tick = Id<AnimationTickId>(2);
        std::array<GraphStateEvent, 262> output;
        output[0].activation = 1234;
        std::uint32_t count = 4321;
        CHECK(instance.CopyStateEvents({}, output, count).failure == GraphInstanceFailure::HandleMalformed);
        CHECK(output[0].activation == 1234);
        CHECK(count == 4321);
        CHECK(instance.Evaluate({tick, {std::numeric_limits<std::int64_t>::max()}, {16'000'000}}).HasError());
        CHECK(instance.Publication().currentPose == initial.outcome.publication.currentPose);
    }

    TEST_CASE("Animation state cuts cannot bypass the active clip traversal budget", "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        StateGraph::Machine(graph).transitions[0].duration = {0};
        StateGraph::Machine(graph).transitions[0].exitTime = AnimationTime{100};
        Prepared fixture(graph);
        fixture.context.clipLimits.maximumBoundaryCrossings = 0;
        auto instance = fixture.Create();
        Tick first(instance, 1, 1);
        const std::array<GraphParameterInput, 1> enter{{{Id<GraphParameterId>(1), 2.0F}}};
        const auto rejected = instance.Evaluate({Id<AnimationTickId>(2), {100}, {}, enter});
        CHECK(rejected.failure == GraphInstanceFailure::ClipFailure);
        CHECK(rejected.clipFailure.failure == AnimationClipFailure::LimitExceeded);
        CHECK(instance.Publication().currentPose == first.outcome.publication.currentPose);
        Tick retry(instance, 2, 1, enter);
        CHECK(retry.pose[0].translation.x == 2.0F);
        CHECK(retry.count == 0);
    }
}  // namespace Horo::Animation::Test
