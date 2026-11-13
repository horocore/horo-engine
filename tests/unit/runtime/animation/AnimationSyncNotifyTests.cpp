#include "AnimationBlendTreeTestFixtures.h"
#include "AnimationClipNotifyTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"

namespace Horo::Animation::Test {
    using namespace Notifies;

    TEST_CASE("Sync notify policy selects actual leader rather than emitting follower alignment travel", "[animation][sync][notifies]") {
        auto tree = BlendTree::Tree();
        tree.sync.events = GraphSyncEventPolicy::LeaderOnly;
        auto graph = BlendTree::Data(tree);
        BlendTree::Prepared fixture(graph);
        Events(fixture, 0, {Event(1, 10)});
        Events(fixture, 1, {Event(2, 20)});
        auto instance = fixture.Create(graph);
        Tick first(instance, 1, 20);
        REQUIRE(first.count == 1);
        CHECK(first.events[0].id.marker == Id<AnimationEventMarkerId>(1));
        const auto coordinates = BlendTree::Coordinates(1.0F);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {5}, {}, coordinates}).HasValue());
        const auto committed = instance.Commit(Id<AnimationTickId>(2));
        REQUIRE(committed.HasValue());
        std::array<AnimationEventOccurrence, 32> output;
        std::uint32_t count = 0;
        REQUIRE(instance.CopyClipEvents(committed.publication.currentPose, output, count).HasValue());
        CHECK(count == 0);
    }

    TEST_CASE("All-active sync notifies deliver adapted follower intervals and exact identities", "[animation][sync][notifies]") {
        auto tree = BlendTree::Tree();
        tree.sync.events = GraphSyncEventPolicy::AllActive;
        const auto graph = BlendTree::Data(tree);
        BlendTree::Prepared fixture(graph);
        Events(fixture, 0, {Event(1, 25)});
        Events(fixture, 1, {Event(2, 50)});
        auto instance = fixture.Create(graph);
        Tick first(instance, 1, 25);
        REQUIRE(first.count == 2);
        CHECK(Count(first, 1, AnimationNotifyPhase::Point) == 1);
        CHECK(Count(first, 2, AnimationNotifyPhase::Point) == 1);
        CHECK(first.events[0].id.player != first.events[1].id.player);
        Tick next(instance, 2, 1);
        CHECK(next.count == 0);
    }

    TEST_CASE("State transition notify suppression closes delivered duration without inventing endpoint travel",
              "[animation][state_graph][notifies]") {
        Instance::Prepared fixture(StateGraph::Data());
        Events(fixture, 0, {Event(1, 5, AnimationDeltaTime{80})});
        fixture.context.clipEvents.transitions = GraphTransitionEventPolicy::SuppressWhileBlending;
        auto instance = fixture.Create();
        Tick first(instance, 1, 10);
        REQUIRE(first.count == 1);
        const std::array<GraphParameterInput, 1> speed{{{Id<GraphParameterId>(1), 2.0F}}};
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {10}, {}, speed}).HasValue());
        const auto committed = instance.Commit(Id<AnimationTickId>(2));
        REQUIRE(committed.HasValue());
        std::array<AnimationEventOccurrence, 16> output;
        std::uint32_t count = 0;
        REQUIRE(instance.CopyClipEvents(committed.publication.currentPose, output, count).HasValue());
        REQUIRE(count == 1);
        CHECK(output[0].id.phase == AnimationNotifyPhase::End);
        CHECK(output[0].reason == AnimationNotifyReason::Suppressed);
        CHECK(output[0].time.ticks == 10);
        CHECK(output[0].windowBeginSerial == first.events[0].id.serial);
    }

    TEST_CASE("Cancelled notify commit preserves old snapshot and exact retry occurrence serial", "[animation][notifies][cancel]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10)});
        auto instance = fixture.Create();
        CancellationSource cancellation;
        const auto tick = Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {20}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(instance.Commit(tick).failure == GraphInstanceFailure::Cancelled);
        CHECK_FALSE(instance.Publication().currentPose.IsValid());
        Tick retry(instance, 1, 20);
        REQUIRE(retry.count == 1);
        CHECK(retry.events[0].id.serial == 1);
    }

    TEST_CASE("Paused sync alignment closes old duration at its true sample without fabricated travel",
              "[animation][sync][notifies][relocation]") {
        auto tree = BlendTree::Tree();
        tree.sync.events = GraphSyncEventPolicy::AllActive;
        const auto graph = BlendTree::Data(tree);
        BlendTree::Prepared fixture(graph);
        auto changed = fixture.compileClips[1].Data();
        changed.descriptor.duration = {101};
        changed.tracks[0].keys.back().time = {101};
        auto clip = AnimationClipAsset::Create(changed, *fixture.skeleton);
        REQUIRE(clip.HasValue());
        fixture.compileClips[1] = std::move(clip).Value();
        fixture.Refresh();
        Events(fixture, 0, {Event(1, 50, AnimationDeltaTime{1})});
        auto instance = fixture.Create(graph);
        Tick first(instance, 1, 50);
        REQUIRE(first.count == 1);
        const auto coordinates = BlendTree::Coordinates(0.75F);
        const GraphEvaluationInput paused{Id<AnimationTickId>(2), {0}, {}, coordinates};
        REQUIRE(instance.Evaluate(paused).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK(instance.Publication().currentPose == first.committed.publication.currentPose);
        REQUIRE(instance.Evaluate(paused).HasValue());
        const auto committed = instance.Commit(paused.tick);
        REQUIRE(committed.HasValue());
        std::array<AnimationEventOccurrence, 8> output;
        std::uint32_t count = 0;
        REQUIRE(instance.CopyClipEvents(committed.publication.currentPose, output, count).HasValue());
        REQUIRE(count == 1);
        CHECK(output[0].id.phase == AnimationNotifyPhase::End);
        CHECK(output[0].id.serial == 2);
        CHECK(output[0].windowBeginSerial == first.events[0].id.serial);
        CHECK(output[0].reason == AnimationNotifyReason::Reentered);
        CHECK(output[0].time.ticks == 50);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(3), {0}, {}, coordinates}).HasValue());
        const auto held = instance.Commit(Id<AnimationTickId>(3));
        REQUIRE(held.HasValue());
        REQUIRE(instance.CopyClipEvents(held.publication.currentPose, output, count).HasValue());
        CHECK(count == 0);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(4), {1}, {}, coordinates}).HasValue());
        const auto travelled = instance.Commit(Id<AnimationTickId>(4));
        REQUIRE(travelled.HasValue());
        REQUIRE(instance.CopyClipEvents(travelled.publication.currentPose, output, count).HasValue());
        REQUIRE(count == 1);
        CHECK(output[0].id.phase == AnimationNotifyPhase::Begin);
        CHECK(output[0].reason == AnimationNotifyReason::Crossed);
        CHECK(output[0].time.ticks == 50);
        CHECK(output[0].id.serial == 3);
    }

}  // namespace Horo::Animation::Test
