#include "AnimationActionTestFixtures.h"

#include <catch2/catch_approx.hpp>
#include <thread>

namespace Horo::Animation::Test {
    using namespace Actions;

    TEST_CASE("Animation actions play real Loop clips once and retain exact terminal request results", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        const auto admitted = instance.QueueAction(Request());
        REQUIRE(admitted.HasValue());
        Tick playing(instance, 1, 10);
        CHECK(playing.pose[0].translation.x == 130);
        CHECK(playing.slots[0].active == admitted.handle);
        Tick completed(instance, 2, 50);
        REQUIRE(completed.completionCount == 1);
        CHECK(completed.completions[0].handle == admitted.handle);
        CHECK(completed.completions[0].reason == GraphActionCompletionReason::Completed);
        CHECK(completed.completions[0].sampleTime.ticks == 80);
        const auto result = instance.QueryAction(admitted.handle);
        REQUIRE(result.HasValue());
        CHECK(result.observation.phase == GraphActionPhase::Terminal);
        Tick later(instance, 3, 10);
        CHECK(later.completionCount == 0);
        CHECK(later.pose[0].translation.x == 70);
        REQUIRE(instance.ReleaseActionResult(admitted.handle).HasValue());
        CHECK(instance.QueryAction(admitted.handle).failure == GraphActionFailure::HandleStale);
    }

    TEST_CASE("Animation named slot occurrences sample one original request cursor once per tick", "[animation][actions]") {
        auto data = Data();
        auto &definition = data.definitions[0];
        auto second = definition.nodes.back();
        second.id = Id<GraphNodeId>(6);
        definition.nodes.push_back(second);
        definition.connections = {Graph::Edge(4, 1, 5, 1), Graph::Edge(5, 2, 6, 1), Graph::Edge(6, 2, 2, 1)};
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick tick(instance, 1, 10);
        CHECK(tick.pose[0].translation.x == 130);
        CHECK(tick.slots[0].sampleTime.ticks == 30);
        CHECK(instance.QueryAction(request.handle).observation.sampleTime.ticks == 30);
    }

    TEST_CASE("Animation action masks preserve the advancing base pose", "[animation][actions]") {
        auto data = Data();
        data.actions[0].mask.defaultWeight = 0;
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick tick(instance, 1, 10);
        CHECK(tick.pose[0].translation.x == 10);
        CHECK(tick.slots[0].sampleTime.ticks == 30);
        CHECK(instance.QueryAction(request.handle).observation.phase == GraphActionPhase::Playing);
    }

    TEST_CASE("Animation action envelopes blend into current base and hold the real terminal pose during fade out",
              "[animation][actions]") {
        auto data = Data();
        data.actions[0].blendIn = {20};
        data.actions[0].blendOut = {20};
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick first(instance, 1, 5);
        CHECK(first.slots[0].weight == 0.25F);
        CHECK(first.pose[0].translation.x == Catch::Approx(35));
        Tick terminal(instance, 2, 55);
        CHECK(terminal.completionCount == 0);
        CHECK(terminal.pose[0].translation.x == 180);
        CHECK(terminal.slots[0].sampleTime.ticks == 80);
        Tick fading(instance, 3, 10);
        CHECK(fading.slots[0].weight == 0.5F);
        CHECK(fading.pose[0].translation.x == Catch::Approx(125));
        Tick done(instance, 4, 10);
        REQUIRE(done.completionCount == 1);
        CHECK(done.completions[0].reason == GraphActionCompletionReason::Completed);
        CHECK(done.pose[0].translation.x == 80);
    }

    TEST_CASE("Animation action FIFO carries exact leftover time into the next admitted request", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        auto first = instance.QueueAction(Request());
        auto second = instance.QueueAction(Request());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        Tick tick(instance, 1, 70);
        REQUIRE(tick.completionCount == 1);
        CHECK(tick.completions[0].handle == first.handle);
        CHECK(tick.slots[0].active == second.handle);
        CHECK(tick.slots[0].sampleTime.ticks == 30);
        CHECK(tick.pose[0].translation.x == 130);
        Tick done(instance, 2, 50);
        REQUIRE(done.completionCount == 1);
        CHECK(done.completions[0].handle == second.handle);
    }

    TEST_CASE("Animation action retained result capacity rejects before request issue and snapshots survive release",
              "[animation][actions]") {
        Prepared prepared;
        prepared.context.actions.requests = 2;
        prepared.context.actions.queuedPerSlot = 2;
        auto instance = prepared.Create();
        auto first = instance.QueueAction(Request());
        auto second = instance.QueueAction(Request());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(instance.QueueAction(Request()).failure == GraphActionFailure::CapacityExceeded);
        Tick done(instance, 1, 150);
        REQUIRE(done.completionCount == 2);
        CHECK(instance.QueueAction(Request()).failure == GraphActionFailure::CapacityExceeded);
        REQUIRE(instance.ReleaseActionResult(first.handle).HasValue());
        auto reused = instance.QueueAction(Request());
        REQUIRE(reused.HasValue());
        CHECK(reused.handle.ordinal > second.handle.ordinal);
        std::array<GraphActionObservation, 2> copied;
        std::uint32_t count = 0;
        REQUIRE(instance.CopyActionCompletions(done.outcome.publication.currentPose, copied, count).HasValue());
        CHECK(copied[0].handle == first.handle);
        CHECK(instance.QueryAction(first.handle).failure == GraphActionFailure::HandleStale);
    }

    TEST_CASE("Animation action play and cancellation intents survive abort and cancelled commit", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        auto first = instance.QueueAction(Request());
        auto second = instance.QueueAction(Request());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(instance.CancelAction(second.handle, GraphActionStopPolicy::Cut).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {70}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK(instance.QueryAction(first.handle).observation.phase == GraphActionPhase::Pending);
        CancellationSource cancellation;
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {10}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(instance.Commit(Id<AnimationTickId>(1)).failure == GraphInstanceFailure::Cancelled);
        Tick retry(instance, 1, 10);
        CHECK(retry.slots[0].sampleTime.ticks == 30);
        REQUIRE(retry.completionCount == 1);
        CHECK(retry.completions[0].handle == second.handle);
        CHECK(retry.completions[0].reason == GraphActionCompletionReason::Cancelled);
    }

    TEST_CASE("Animation authored interruption retains a real last committed masked overlay", "[animation][actions]") {
        auto data = Data();
        data.actions[0].blendOut = {20};
        AddSecond(data);
        data.actions[1].blendIn = {20};
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto first = instance.QueueAction(Request());
        REQUIRE(first.HasValue());
        Tick initial(instance, 1, 10);
        const auto next = instance.QueueAction(Request(20, GraphActionQueuePolicy::Interrupt));
        REQUIRE(next.HasValue());
        Tick handoff(instance, 2, 5);
        REQUIRE(handoff.completionCount == 1);
        CHECK(handoff.completions[0].reason == GraphActionCompletionReason::Interrupted);
        CHECK(handoff.pose[0].translation.x == Catch::Approx(77.1875F));
        REQUIRE(instance.ReleaseActionResult(first.handle).HasValue());
        Tick after(instance, 3, 15);
        CHECK(after.pose[0].translation.x == 20);
        CHECK(after.slots[0].active == next.handle);
    }

    TEST_CASE("Animation uninterruptible actions reject new interrupt intents with an exact committed result", "[animation][actions]") {
        auto data = Data();
        data.actions[0].interruption = GraphActionInterruption::Reject;
        AddSecond(data);
        Prepared prepared(data);
        auto instance = prepared.Create();
        REQUIRE(instance.QueueAction(Request()).HasValue());
        Tick first(instance, 1, 10);
        const auto rejected = instance.QueueAction(Request(20, GraphActionQueuePolicy::Interrupt));
        REQUIRE(rejected.HasValue());
        Tick tick(instance, 2, 10);
        REQUIRE(tick.completionCount == 1);
        CHECK(tick.completions[0].handle == rejected.handle);
        CHECK(tick.completions[0].reason == GraphActionCompletionReason::Rejected);
        CHECK(tick.slots[0].sampleTime.ticks == 40);
    }

    TEST_CASE("Animation request cancellation uses authored fade and explicit Cut without clip notifies", "[animation][actions]") {
        auto data = Data();
        data.actions[0].blendOut = {20};
        Prepared prepared(data);
        auto instance = prepared.Create();
        auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick initial(instance, 1, 10);
        REQUIRE(instance.CancelAction(request.handle).HasValue());
        Tick fading(instance, 2, 5);
        CHECK(fading.slots[0].weight == 0.75F);
        CHECK(fading.slots[0].sampleTime.ticks == 30);
        REQUIRE(instance.CancelAction(request.handle, GraphActionStopPolicy::Cut).HasValue());
        Tick stopped(instance, 3, 1);
        REQUIRE(stopped.completionCount == 1);
        CHECK(stopped.completions[0].reason == GraphActionCompletionReason::Cancelled);
    }

    TEST_CASE("Animation action commands enforce owner pending and exact stale handle gates", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        CHECK(instance.ReleaseActionResult(request.handle).failure == GraphActionFailure::NotTerminal);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {1}}).HasValue());
        CHECK(instance.CancelAction(request.handle).failure == GraphActionFailure::EvaluationPending);
        REQUIRE(instance.Abort().HasValue());
        GraphActionCommandOutcome foreign;
        std::jthread worker([&] {
            foreign = instance.CancelAction(request.handle);
        });
        worker.join();
        CHECK(foreign.failure == GraphActionFailure::ThreadViolation);
        auto stale = request.handle;
        ++stale.instance.slot.generation;
        CHECK(instance.CancelAction(stale).failure == GraphActionFailure::HandleStale);
    }

    TEST_CASE("Animation actions shutdown cancels actual owner records without modifying prior pose snapshots", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick playing(instance, 1, 10);
        REQUIRE(instance.Shutdown().HasValue());
        const auto result = instance.QueryAction(request.handle);
        REQUIRE(result.HasValue());
        CHECK(result.observation.reason == GraphActionCompletionReason::Shutdown);
        CHECK(instance.QueueAction(Request()).failure == GraphActionFailure::Closed);
        std::array<GraphActionSlotObservation, 1> slots;
        std::uint32_t count = 0;
        REQUIRE(instance.CopyActionSlots(playing.outcome.publication.currentPose, slots, count).HasValue());
        CHECK(slots[0].active == request.handle);
        REQUIRE(instance.ReleaseActionResult(request.handle).HasValue());
        CHECK(instance.QueryAction(request.handle).failure == GraphActionFailure::HandleStale);
    }

    TEST_CASE("Animation action complete command evaluation publication and shutdown hot paths allocate nothing", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        const auto request = Request();
        const auto tick = Id<AnimationTickId>(1);
        std::array<GraphActionSlotObservation, 1> slots;
        std::array<GraphActionObservation, 64> completions;
        std::uint32_t count = 0;
        const auto before = HoroAnimationTestAllocationCount();
        const auto admitted = instance.QueueAction(request);
        const auto evaluated = instance.Evaluate({tick, {100}});
        const auto committed = instance.Commit(tick);
        const auto queried = instance.QueryAction(admitted.handle);
        const auto copied = instance.CopyActionSlots(committed.publication.currentPose, slots, count);
        const auto completed = instance.CopyActionCompletions(committed.publication.currentPose, completions, count);
        const auto released = instance.ReleaseActionResult(admitted.handle);
        const auto shutdown = instance.Shutdown();
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(admitted.HasValue());
        CHECK(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(queried.HasValue());
        CHECK(copied.HasValue());
        CHECK(completed.HasValue());
        CHECK(released.HasValue());
        CHECK(shutdown.HasValue());
    }

    TEST_CASE("Animation named slots own distinct queues and original cursors", "[animation][actions]") {
        auto data = Data();
        AddSecond(data);
        data.actions[1].slot = Id<GraphActionSlotId>(2);
        data.actionSlots.push_back({Id<GraphActionSlotId>(2), "LowerBody"});
        auto node = data.definitions[0].nodes.back();
        node.id = Id<GraphNodeId>(6);
        node.payload = GraphActionSlotNode{Id<GraphActionSlotId>(2)};
        data.definitions[0].nodes.push_back(node);
        data.definitions[0].connections = {Graph::Edge(4, 1, 5, 1), Graph::Edge(5, 2, 6, 1), Graph::Edge(6, 2, 2, 1)};
        Prepared prepared(data);
        auto instance = prepared.Create();
        auto upper = instance.QueueAction(Request());
        auto lower = instance.QueueAction(Request(20));
        REQUIRE(upper.HasValue());
        REQUIRE(lower.HasValue());
        Tick first(instance, 1, 10);
        REQUIRE(first.slotCount == 2);
        CHECK(first.slots[0].sampleTime.ticks == 30);
        CHECK(first.slots[1].sampleTime.ticks == 10);
        REQUIRE(instance.CancelAction(lower.handle, GraphActionStopPolicy::Cut).HasValue());
        Tick next(instance, 2, 10);
        CHECK(next.pose[0].translation.x == 140);
        CHECK(next.slots[0].active == upper.handle);
        CHECK_FALSE(next.slots[1].active.IsValid());
    }

    TEST_CASE("Animation action admission cannot overrun its own cold command-processing budget", "[animation][actions]") {
        Prepared prepared;
        prepared.context.actions.transitionsPerTick = 1;
        auto instance = prepared.Create();
        REQUIRE(instance.QueueAction(Request()).HasValue());
        CHECK(instance.QueueAction(Request()).failure == GraphActionFailure::CapacityExceeded);
        Tick admitted(instance, 1, 0);
        CHECK(admitted.slots[0].sampleTime.ticks == 20);
    }

    TEST_CASE("Animation exact-boundary queued heads keep FIFO when new requests arrive and aborted attempts retry",
              "[animation][actions][queue]") {
        auto data = Data();
        AddSecond(data);
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto first = instance.QueueAction(Request());
        const auto waiting = instance.QueueAction(Request());
        REQUIRE(first.HasValue());
        REQUIRE(waiting.HasValue());
        Tick boundary(instance, 1, 60);
        REQUIRE(boundary.completionCount == 1);
        CHECK_FALSE(boundary.slots[0].active.IsValid());
        CHECK(boundary.slots[0].queued == 1);
        const auto arriving = instance.QueueAction(Request(20));
        REQUIRE(arriving.HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {10}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK(instance.QueryAction(waiting.handle).observation.phase == GraphActionPhase::Queued);
        CHECK(instance.QueryAction(arriving.handle).observation.phase == GraphActionPhase::Pending);
        Tick retry(instance, 2, 10);
        CHECK(retry.slots[0].active == waiting.handle);
        CHECK(retry.slots[0].sampleTime.ticks == 30);
        CHECK(retry.slots[0].queued == 1);
        Tick nextBoundary(instance, 3, 50);
        REQUIRE(nextBoundary.completionCount == 1);
        CHECK(nextBoundary.completions[0].handle == waiting.handle);
        Tick last(instance, 4, 10);
        CHECK(last.slots[0].active == arriving.handle);
        CHECK(last.slots[0].sampleTime.ticks == 10);
    }

    TEST_CASE("Animation exact-boundary queued work is busy for IfIdle and explicit interrupt priority stays distinct",
              "[animation][actions][queue]") {
        auto data = Data();
        AddSecond(data);
        Prepared prepared(data);
        auto instance = prepared.Create();
        REQUIRE(instance.QueueAction(Request()).HasValue());
        const auto waiting = instance.QueueAction(Request());
        REQUIRE(waiting.HasValue());
        Tick boundary(instance, 1, 60);
        SECTION("IfIdle rejects instead of overtaking") {
            const auto arriving = instance.QueueAction(Request(20, GraphActionQueuePolicy::IfIdle));
            REQUIRE(arriving.HasValue());
            Tick tick(instance, 2, 10);
            CHECK(tick.slots[0].active == waiting.handle);
            REQUIRE(tick.completionCount == 1);
            CHECK(tick.completions[0].handle == arriving.handle);
            CHECK(tick.completions[0].reason == GraphActionCompletionReason::Rejected);
        }
        SECTION("explicit interrupt can prioritize over an ordinary waiting head") {
            const auto arriving = instance.QueueAction(Request(20, GraphActionQueuePolicy::Interrupt));
            REQUIRE(arriving.HasValue());
            Tick tick(instance, 2, 10);
            CHECK(tick.slots[0].active == arriving.handle);
            CHECK(tick.slots[0].queued == 1);
            CHECK(instance.QueryAction(waiting.handle).observation.phase == GraphActionPhase::Queued);
        }
    }

    TEST_CASE("Animation a reserved boundary interrupt remains first after the former active action fades out",
              "[animation][actions][queue]") {
        auto data = Data();
        data.actions[0].blendOut = {20};
        data.actions[0].interruption = GraphActionInterruption::AtSectionBoundary;
        AddSecond(data);
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto active = instance.QueueAction(Request());
        REQUIRE(active.HasValue());
        Tick initial(instance, 1, 10);
        const auto reserved = instance.QueueAction(Request(20, GraphActionQueuePolicy::Interrupt));
        REQUIRE(reserved.HasValue());
        REQUIRE(instance.CancelAction(active.handle).HasValue());
        Tick faded(instance, 2, 20);
        CHECK_FALSE(faded.slots[0].active.IsValid());
        CHECK(faded.slots[0].queued == 1);
        const auto arriving = instance.QueueAction(Request(10, GraphActionQueuePolicy::Interrupt));
        REQUIRE(arriving.HasValue());
        Tick tick(instance, 3, 5);
        CHECK(tick.slots[0].active == reserved.handle);
        CHECK(tick.slots[0].sampleTime.ticks == 5);
        REQUIRE(tick.completionCount == 1);
        CHECK(tick.completions[0].handle == arriving.handle);
        CHECK(tick.completions[0].reason == GraphActionCompletionReason::Rejected);
    }

}  // namespace Horo::Animation::Test
