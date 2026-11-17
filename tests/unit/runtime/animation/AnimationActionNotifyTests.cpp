#include "AnimationClipNotifyTestFixtures.h"

namespace Horo::Animation::Test {
    using namespace Notifies;

    TEST_CASE("Action notifies consume real region leftovers across section and FIFO boundaries", "[animation][actions][notifies]") {
        auto graph = Actions::Data();
        graph.actions[0].sequence.sections[0].end = {40};
        graph.actions[0].sequence.sections[0].next = Id<GraphActionSectionId>(2);
        graph.actions[0].sequence.sections.push_back({Id<GraphActionSectionId>(2), "End", Asset<AnimationClipId>(4), {60}, {80}});
        Actions::Prepared fixture(graph);
        Events(fixture, 1, {Event(1, 30), Event(2, 50), Event(3, 70), Event(4, 25, AnimationDeltaTime{50})});
        auto instance = fixture.Create();
        const auto first = instance.QueueAction(Actions::Request());
        const auto second = instance.QueueAction(Actions::Request());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        Tick tick(instance, 1, 50);
        CHECK(Count(tick, 1, AnimationNotifyPhase::Point) == 2);
        CHECK(Count(tick, 2, AnimationNotifyPhase::Point) == 0);
        CHECK(Count(tick, 3, AnimationNotifyPhase::Point) == 1);
        CHECK(Count(tick, 4, AnimationNotifyPhase::Begin) == 3);
        CHECK(Count(tick, 4, AnimationNotifyPhase::End) == 2);
        CHECK(tick.events[tick.count - 1].action == second.handle);
        CHECK(instance.QueryAction(first.handle).observation.reason == GraphActionCompletionReason::Completed);
    }

    TEST_CASE("Action duration cancellation closes at actual held sample and abort preserves window ownership",
              "[animation][actions][notifies]") {
        Actions::Prepared fixture;
        Events(fixture, 1, {Event(1, 25, AnimationDeltaTime{50})});
        auto instance = fixture.Create();
        const auto request = instance.QueueAction(Actions::Request());
        REQUIRE(request.HasValue());
        Tick first(instance, 1, 10);
        REQUIRE(first.count == 1);
        REQUIRE(instance.CancelAction(request.handle, GraphActionStopPolicy::Cut).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {5}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        Tick retry(instance, 2, 5);
        REQUIRE(retry.count == 1);
        CHECK(retry.events[0].id.phase == AnimationNotifyPhase::End);
        CHECK(retry.events[0].reason == AnimationNotifyReason::Cancelled);
        CHECK(retry.events[0].time.ticks == 30);
        CHECK(retry.events[0].windowBeginSerial == first.events[0].id.serial);
        CHECK(retry.events[0].id.serial == first.events[0].id.serial + 1);
        Tick next(instance, 3, 5);
        CHECK(next.count == 0);
    }

    TEST_CASE("Action notify loop regions preserve delivered duration identity and explicit release", "[animation][actions][notifies]") {
        auto graph = Actions::Data();
        auto &action = graph.actions[0];
        action.sequence.sections[0].end = {40};
        action.sequence.loops = {
            {Id<GraphActionLoopId>(1), Id<GraphActionSectionId>(1), Id<GraphActionSectionId>(1), GraphActionLoopMode::UntilReleased, 0}};
        Actions::Prepared fixture(graph);
        Events(fixture, 1, {Event(1, 20), Event(2, 25, AnimationDeltaTime{50})});
        auto instance = fixture.Create();
        const auto request = instance.QueueAction(Actions::Request());
        REQUIRE(request.HasValue());
        Tick repeated(instance, 1, 45);
        CHECK(Count(repeated, 1, AnimationNotifyPhase::Point) == 3);
        CHECK(Count(repeated, 2, AnimationNotifyPhase::Begin) == 3);
        CHECK(Count(repeated, 2, AnimationNotifyPhase::End) == 2);
        REQUIRE(instance.ReleaseActionLoop(request.handle).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {15}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        Tick released(instance, 2, 15);
        REQUIRE(released.count == 1);
        CHECK(released.events[0].id.phase == AnimationNotifyPhase::End);
        CHECK(released.events[0].reason == AnimationNotifyReason::PlaybackEnd);
        CHECK(released.events[0].id.loopOrdinal == 2);
        CHECK(instance.QueryAction(request.handle).observation.phase == GraphActionPhase::Terminal);
    }

    TEST_CASE("Held and paused action notifies retain one duration owner without replaying activation points",
              "[animation][actions][notifies][held]") {
        Actions::Prepared fixture;
        Events(fixture, 1, {Event(1, 25, AnimationDeltaTime{50}), Event(2, 30)});
        auto instance = fixture.Create();
        REQUIRE(instance.QueueAction(Actions::Request()).HasValue());
        Tick first(instance, 1, 10);
        REQUIRE(first.count == 2);
        const auto beginSerial = first.events[0].id.serial;
        Tick zero(instance, 2, 0);
        CHECK(zero.count == 0);
        Tick paused(instance, 3, 50, {0});
        CHECK(paused.count == 0);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(4), {0}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        Tick retry(instance, 4, 0);
        CHECK(retry.count == 0);
        Tick resumed(instance, 5, 1);
        CHECK(resumed.count == 0);
        Tick ended(instance, 6, 44);
        REQUIRE(ended.count == 1);
        CHECK(ended.events[0].id.phase == AnimationNotifyPhase::End);
        CHECK(ended.events[0].time.ticks == 75);
        CHECK(ended.events[0].reason == AnimationNotifyReason::Crossed);
        CHECK(ended.events[0].windowBeginSerial == beginSerial);
        CHECK(ended.events[0].id.serial == 3);
    }

    TEST_CASE("Exact action loop seam publishes entry ownership even without a following travelled segment",
              "[animation][actions][notifies][seam]") {
        auto graph = Actions::Data();
        graph.actions[0].sequence.sections[0].end = {40};
        graph.actions[0].sequence.loops = {
            {Id<GraphActionLoopId>(1), Id<GraphActionSectionId>(1), Id<GraphActionSectionId>(1), GraphActionLoopMode::UntilReleased, 0}};
        Actions::Prepared fixture(graph);
        Events(fixture, 1, {Event(1, 20), Event(2, 20, AnimationDeltaTime{55})});
        auto exact = fixture.Create();
        auto remainder = fixture.Create();
        REQUIRE(exact.QueueAction(Actions::Request()).HasValue());
        REQUIRE(remainder.QueueAction(Actions::Request()).HasValue());
        REQUIRE(exact.Evaluate({Id<AnimationTickId>(1), {20}}).HasValue());
        REQUIRE(exact.Abort().HasValue());
        Tick seam(exact, 1, 20);
        Tick carried(remainder, 1, 21);
        REQUIRE(seam.count == 5);
        REQUIRE(carried.count == seam.count);
        for (std::uint32_t i = 0; i < seam.count; ++i)
            CHECK(seam.events[i] == carried.events[i]);
        CHECK(Count(seam, 1, AnimationNotifyPhase::Point) == 2);
        CHECK(Count(seam, 2, AnimationNotifyPhase::Begin) == 2);
        CHECK(Count(seam, 2, AnimationNotifyPhase::End) == 1);
        CHECK(seam.events[seam.count - 1].id.loopOrdinal == 1);
        Tick held(exact, 2, 0);
        CHECK(held.count == 0);
        Tick paused(exact, 3, 10, {0});
        CHECK(paused.count == 0);
        Tick continued(exact, 4, 1);
        CHECK(continued.count == 0);
    }

    TEST_CASE("Reverse action loop seam retains its directed ordinal through a held boundary", "[animation][actions][notifies][reverse]") {
        auto graph = Actions::Data();
        graph.actions[0].reverse = GraphActionReverse::Allow;
        graph.actions[0].sequence.sections[0].end = {40};
        graph.actions[0].sequence.loops = {
            {Id<GraphActionLoopId>(1), Id<GraphActionSectionId>(1), Id<GraphActionSectionId>(1), GraphActionLoopMode::UntilReleased, 0}};
        Actions::Prepared fixture(graph);
        Events(fixture, 1, {Event(1, 40), Event(2, 0, AnimationDeltaTime{40})});
        auto instance = fixture.Create();
        const auto request = instance.QueueAction(Actions::Request());
        REQUIRE(request.HasValue());
        Tick seam(instance, 1, 20, {-1'000'000});
        REQUIRE(seam.count == 5);
        CHECK(seam.events[seam.count - 1].id.loopOrdinal == -1);
        CHECK(Count(seam, 1, AnimationNotifyPhase::Point) == 2);
        Tick held(instance, 2, 0, {-1'000'000});
        CHECK(held.count == 0);
        REQUIRE(instance.ReleaseActionLoop(request.handle).HasValue());
        Tick released(instance, 3, 20, {-1'000'000});
        REQUIRE(released.count == 1);
        CHECK(released.events[0].id.loopOrdinal == -1);
        CHECK(released.events[0].reason == AnimationNotifyReason::PlaybackEnd);
        CHECK(released.events[0].time.ticks == 20);
    }

}  // namespace Horo::Animation::Test
