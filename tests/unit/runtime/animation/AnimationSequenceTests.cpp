#include "AnimationActionTestFixtures.h"

#include <catch2/catch_approx.hpp>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Animation::Test {
    using namespace Actions;

    namespace {
        /** @brief Intro, explicit loop region and a real second-clip outro share exact leftover time. */
        AnimationGraphData Sequence(const GraphActionLoopMode mode = GraphActionLoopMode::Finite) {
            auto data = Data();
            auto &sequence = data.actions[0].sequence;
            sequence.sections = {{Id<GraphActionSectionId>(1), "Intro", Asset<AnimationClipId>(4), {0}, {20}, Id<GraphActionSectionId>(2)},
                                 {Id<GraphActionSectionId>(2), "Loop", Asset<AnimationClipId>(4), {20}, {40}, Id<GraphActionSectionId>(3)},
                                 {Id<GraphActionSectionId>(3), "Outro", Asset<AnimationClipId>(3), {50}, {70}}};
            sequence.loops = {{Id<GraphActionLoopId>(1), Id<GraphActionSectionId>(2), Id<GraphActionSectionId>(2), mode,
                               mode == GraphActionLoopMode::Finite ? 3U : 0U}};
            return data;
        }
    }  // namespace

    TEST_CASE("Animation sequences consume finite loop regions and cross-clip sections without losing time",
              "[animation][actions][sequence]") {
        Prepared prepared(Sequence());
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick crossed(instance, 1, 85);
        CHECK(crossed.slots[0].section == Id<GraphActionSectionId>(3));
        CHECK(crossed.slots[0].sampleTime.ticks == 55);
        CHECK(crossed.pose[0].translation.x == 55);
        Tick completed(instance, 2, 15);
        REQUIRE(completed.completionCount == 1);
        CHECK(completed.completions[0].sampleTime.ticks == 70);
        CHECK(completed.completions[0].handle == request.handle);
    }

    TEST_CASE("Animation release-loop intent exits at the authored boundary and survives aborted attempts",
              "[animation][actions][sequence]") {
        Prepared prepared(Sequence(GraphActionLoopMode::UntilReleased));
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick looped(instance, 1, 55);
        CHECK(looped.slots[0].sampleTime.ticks == 35);
        REQUIRE(instance.ReleaseActionLoop(request.handle).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {10}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK(instance.QueryAction(request.handle).observation.sampleTime.ticks == 35);
        Tick exited(instance, 2, 10);
        CHECK(exited.slots[0].section == Id<GraphActionSectionId>(3));
        CHECK(exited.slots[0].sampleTime.ticks == 55);
        Tick completed(instance, 3, 15);
        REQUIRE(completed.completionCount == 1);
        CHECK(completed.completions[0].reason == GraphActionCompletionReason::Completed);
    }

    TEST_CASE("Animation sequence transition budget rejects the complete large interval and retry preserves every cursor",
              "[animation][actions][sequence]") {
        Prepared prepared(Sequence());
        prepared.context.actions.transitionsPerTick = 2;
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        CHECK(instance.Evaluate({Id<AnimationTickId>(1), {85}}).failure == GraphInstanceFailure::LimitExceeded);
        CHECK_FALSE(instance.Publication().currentPose.IsValid());
        CHECK(instance.QueryAction(request.handle).observation.phase == GraphActionPhase::Pending);
        Tick retry(instance, 1, 5);
        CHECK(retry.slots[0].sampleTime.ticks == 5);
        CHECK(retry.pose[0].translation.x == 105);
    }

    TEST_CASE("Animation section-boundary interruption starts the actual replacement with exact leftover time",
              "[animation][actions][sequence]") {
        auto data = Data();
        data.actions[0].interruption = GraphActionInterruption::AtSectionBoundary;
        auto &first = data.actions[0].sequence.sections[0];
        first.end = {40};
        first.next = Id<GraphActionSectionId>(2);
        data.actions[0].sequence.sections.push_back({Id<GraphActionSectionId>(2), "End", Asset<AnimationClipId>(4), {40}, {80}});
        AddSecond(data);
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto old = instance.QueueAction(Request());
        REQUIRE(old.HasValue());
        Tick initial(instance, 1, 10);
        const auto next = instance.QueueAction(Request(20, GraphActionQueuePolicy::Interrupt));
        REQUIRE(next.HasValue());
        Tick transitioned(instance, 2, 15);
        REQUIRE(transitioned.completionCount == 1);
        CHECK(transitioned.completions[0].reason == GraphActionCompletionReason::Interrupted);
        CHECK(transitioned.slots[0].active == next.handle);
        CHECK(transitioned.slots[0].sampleTime.ticks == 5);
    }

    TEST_CASE("Animation sequence authored reverse playback and already-reached region edges stay bounded",
              "[animation][actions][sequence]") {
        auto data = Data();
        data.actions[0].reverse = GraphActionReverse::Allow;
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick reversed(instance, 1, 10, GraphPlaybackRate{-1'000'000});
        CHECK(reversed.slots[0].sampleTime.ticks == 70);
        CHECK(reversed.pose[0].translation.x == 170);
        Tick done(instance, 2, 50, GraphPlaybackRate{-1'000'000});
        REQUIRE(done.completionCount == 1);
        CHECK(done.completions[0].sampleTime.ticks == 20);
        auto edge = prepared.Create();
        REQUIRE(edge.QueueAction(Request()).HasValue());
        Tick paused(edge, 1, 0);
        Tick atEdge(edge, 2, 1, GraphPlaybackRate{-1'000'000});
        REQUIRE(atEdge.completionCount == 1);
        CHECK(atEdge.completions[0].sampleTime.ticks == 20);
    }

    TEST_CASE("Animation actions reject unsupported reverse without consuming the admitted request", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        CHECK(instance.Evaluate({Id<AnimationTickId>(1), {10}, GraphPlaybackRate{-1'000'000}}).failure ==
              GraphInstanceFailure::TypeMismatch);
        CHECK(instance.QueryAction(request.handle).observation.phase == GraphActionPhase::Pending);
        Tick retry(instance, 1, 10);
        CHECK(retry.slots[0].sampleTime.ticks == 30);
    }

    TEST_CASE("Animation action queue capacity produces an explicit retained rejection rather than truncation", "[animation][actions]") {
        Prepared prepared;
        prepared.context.actions.queuedPerSlot = 1;
        auto instance = prepared.Create();
        REQUIRE(instance.QueueAction(Request()).HasValue());
        REQUIRE(instance.QueueAction(Request()).HasValue());
        const auto rejected = instance.QueueAction(Request());
        REQUIRE(rejected.HasValue());
        Tick tick(instance, 1, 10);
        CHECK(tick.slots[0].queued == 1);
        REQUIRE(tick.completionCount == 1);
        CHECK(tick.completions[0].handle == rejected.handle);
        CHECK(tick.completions[0].reason == GraphActionCompletionReason::Rejected);
    }

    TEST_CASE("Animation request cancellation token is an actual owner intent and not a clip notify", "[animation][actions]") {
        Prepared prepared;
        auto instance = prepared.Create();
        CancellationSource cancellation;
        auto request = Request();
        request.cancellation = cancellation.Token();
        const auto handle = instance.QueueAction(request);
        REQUIRE(handle.HasValue());
        Tick active(instance, 1, 10);
        cancellation.RequestCancellation();
        Tick cancelled(instance, 2, 1);
        REQUIRE(cancelled.completionCount == 1);
        CHECK(cancelled.completions[0].reason == GraphActionCompletionReason::Cancelled);
    }

    TEST_CASE("Animation action source round trips exact named sequence policies and immutable clip dependencies",
              "[animation][actions][source]") {
        auto data = Sequence();
        data.actions[0].blendIn = {10};
        data.actions[0].blendOut = {20};
        data.actions[0].mask.joints = {{Id<JointId>(1), 0.5F}};
        Prepared prepared(data);
        auto encoded = SerializeAnimationGraphSource(data);
        REQUIRE(encoded.HasValue());
        const auto decoded = DeserializeAnimationGraphSource(encoded.Value(), data.id);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == MigrateAnimationGraph(data).Value());
        const auto compiled = AnimationGraphProgram::Compile(decoded.Value(), prepared.CompileContext());
        REQUIRE(compiled.HasValue());
        CHECK(compiled.Value().Actions().size() == 1);
        CHECK(compiled.Value().ActionSlots().size() == 1);
        const auto again = SerializeAnimationGraphSource(decoded.Value());
        REQUIRE(again.HasValue());
        CHECK(again.Value() == encoded.Value());
    }

    TEST_CASE("Animation action catalog rejects malformed sections loops masks and slot bindings", "[animation][actions][validation]") {
        auto data = Sequence();
        Prepared prepared(data);
        SECTION("missing slot") {
            data.actions[0].slot = Id<GraphActionSlotId>(99);
        }
        SECTION("empty range") {
            data.actions[0].sequence.sections[0].end = {0};
        }
        SECTION("out of publication") {
            data.actions[0].sequence.sections[0].end = {101};
        }
        SECTION("unknown next") {
            data.actions[0].sequence.sections[0].next = Id<GraphActionSectionId>(99);
        }
        SECTION("ordinary cycle") {
            data.actions[0].sequence.sections[2].next = Id<GraphActionSectionId>(1);
        }
        SECTION("duplicate section") {
            data.actions[0].sequence.sections.push_back(data.actions[0].sequence.sections[0]);
        }
        SECTION("overlap loop") {
            auto loop = data.actions[0].sequence.loops[0];
            loop.id = Id<GraphActionLoopId>(2);
            data.actions[0].sequence.loops.push_back(loop);
        }
        SECTION("missing loop end") {
            data.actions[0].sequence.loops[0].last = Id<GraphActionSectionId>(99);
        }
        SECTION("bad mask") {
            data.actions[0].mask.defaultWeight = std::numeric_limits<float>::quiet_NaN();
        }
        CHECK(AnimationGraphProgram::Compile(data, prepared.CompileContext()).HasError());
    }

    TEST_CASE("Animation action reload revokes request handles and failed reload preserves last good owner state",
              "[animation][actions][reload]") {
        Prepared old;
        auto instance = old.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick prior(instance, 1, 10);
        Prepared next(Data(), 7);
        auto context = next.context;
        context.instance = old.context.instance;
        ++context.instance.slot.generation;
        auto stale = next.Assets();
        stale.program = old.program;
        CHECK(instance.Replace(stale, context).HasError());
        CHECK(instance.QueryAction(request.handle).observation.phase == GraphActionPhase::Playing);
        REQUIRE(instance.Replace(next.Assets(), context).HasValue());
        CHECK(instance.QueryAction(request.handle).failure == GraphActionFailure::HandleStale);
        std::array<GraphActionSlotObservation, 1> output;
        std::uint32_t count = 0;
        CHECK(instance.CopyActionSlots(prior.outcome.publication.currentPose, output, count).failure == GraphInstanceFailure::HandleStale);
    }

    TEST_CASE("Animation action cold storage and command bounds reject before activation", "[animation][actions][admission]") {
        Prepared prepared;
        auto context = prepared.context;
        SECTION("storage") {
            context.maximumStorageBytes = 1;
        }
        SECTION("request limit") {
            context.actions.requests = 0;
        }
        SECTION("queue limit") {
            context.actions.queuedPerSlot = context.actions.requests + 1;
        }
        SECTION("transition limit") {
            context.actions.transitionsPerTick = 0;
        }
        CHECK(AnimationGraphInstance::Create(prepared.Assets(), context).HasError());
    }

    TEST_CASE("Animation schema five graphs migrate explicitly without gaining action authority", "[animation][actions][migration]") {
        auto plain = Graph::Simple();
        auto encoded = SerializeAnimationGraphSource(plain);
        REQUIRE(encoded.HasValue());
        auto legacy = nlohmann::json::parse(encoded.Value());
        legacy["schemaVersion"] = 5;
        legacy["contractVersion"] = {{"major", 1}, {"minor", 3}, {"patch", 0}};
        legacy["payload"].erase("actionSlots");
        legacy["payload"].erase("actions");
        CHECK(DeserializeAnimationGraphSource(legacy.dump(), plain.id).HasError());
        const auto migrated =
            DeserializeAnimationGraphSource(legacy.dump(), plain.id, {}, {}, AnimationGraphSourceMigration::MigrateSupported);
        REQUIRE(migrated.HasValue());
        CHECK(migrated.Value().actions.empty());
        auto forbidden = Data();
        forbidden.schemaVersion = 5;
        forbidden.contractVersion = {1, 3, 0};
        CHECK(MigrateAnimationGraph(forbidden).HasError());
    }

    TEST_CASE("Animation loop release abort cancellation and retained result release allocate no hot memory",
              "[animation][actions][sequence]") {
        Prepared prepared(Sequence(GraphActionLoopMode::UntilReleased));
        auto instance = prepared.Create();
        const auto request = instance.QueueAction(Request());
        REQUIRE(request.HasValue());
        Tick initial(instance, 1, 55);
        const auto second = Id<AnimationTickId>(2), third = Id<AnimationTickId>(3);
        const auto before = HoroAnimationTestAllocationCount();
        const auto released = instance.ReleaseActionLoop(request.handle);
        const auto attempted = instance.Evaluate({second, {10}});
        const auto aborted = instance.Abort();
        const auto retry = instance.Evaluate({second, {10}});
        const auto committed = instance.Commit(second);
        const auto cancelled = instance.CancelAction(request.handle, GraphActionStopPolicy::Cut);
        const auto stopped = instance.Evaluate({third, {0}});
        const auto final = instance.Commit(third);
        const auto result = instance.QueryAction(request.handle);
        const auto retired = instance.ReleaseActionResult(request.handle);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(released.HasValue());
        CHECK(attempted.HasValue());
        CHECK(aborted.HasValue());
        CHECK(retry.HasValue());
        CHECK(committed.HasValue());
        CHECK(cancelled.HasValue());
        CHECK(stopped.HasValue());
        CHECK(final.HasValue());
        CHECK(result.HasValue());
        CHECK(retired.HasValue());
    }

    TEST_CASE("Animation queued release-loop intent survives earlier commits and an aborted activation attempt",
              "[animation][actions][sequence]") {
        auto data = Data();
        auto loop = Sequence(GraphActionLoopMode::UntilReleased).actions[0];
        loop.id = Id<GraphActionId>(20);
        loop.name = "QueuedLoop";
        data.actions.push_back(loop);
        Prepared prepared(data);
        auto instance = prepared.Create();
        const auto first = instance.QueueAction(Request());
        const auto waiting = instance.QueueAction(Request(20));
        REQUIRE(first.HasValue());
        REQUIRE(waiting.HasValue());
        REQUIRE(instance.ReleaseActionLoop(waiting.handle).HasValue());
        Tick prior(instance, 1, 10);
        CHECK(instance.QueryAction(waiting.handle).observation.phase == GraphActionPhase::Queued);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {110}}).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK(instance.QueryAction(waiting.handle).observation.phase == GraphActionPhase::Queued);
        Tick retry(instance, 2, 110);
        REQUIRE(retry.completionCount == 2);
        CHECK(retry.completions[0].handle == first.handle);
        CHECK(retry.completions[1].handle == waiting.handle);
        CHECK(retry.completions[1].reason == GraphActionCompletionReason::Completed);
        CHECK(retry.completions[1].sampleTime.ticks == 70);
        const auto retained = instance.QueryAction(waiting.handle);
        REQUIRE(retained.HasValue());
        CHECK(retained.observation.phase == GraphActionPhase::Terminal);
    }

}  // namespace Horo::Animation::Test
