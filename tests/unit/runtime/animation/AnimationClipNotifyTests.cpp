#include "AnimationClipNotifyTestFixtures.h"

#include <thread>

namespace Horo::Animation::Test {
    using namespace Notifies;

    TEST_CASE("Clip notifies traverse multiple loops and exact seams without duplicate zero events", "[animation][notifies]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 0), Event(2, 100), Event(3, 25), Event(4, 40, AnimationDeltaTime{20})});
        auto instance = fixture.Create();
        Tick tick(instance, 1, 250);
        CHECK(Count(tick, 1, AnimationNotifyPhase::Point) == 3);
        CHECK(Count(tick, 2, AnimationNotifyPhase::Point) == 2);
        CHECK(Count(tick, 3, AnimationNotifyPhase::Point) == 3);
        CHECK(Count(tick, 4, AnimationNotifyPhase::Begin) == 3);
        CHECK(Count(tick, 4, AnimationNotifyPhase::End) == 2);
        Tick next(instance, 2, 10);
        REQUIRE(next.count == 1);
        CHECK(next.events[0].id.phase == AnimationNotifyPhase::End);
        CHECK(next.events[0].windowBeginSerial != 0);
        CHECK(next.events[0].id.loopOrdinal == 2);
    }

    TEST_CASE("Clip notify candidates and occurrence identities survive abort and queue backpressure atomically", "[animation][notifies]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10)});
        Subscription subscription(fixture.context, 1);
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {15}}).HasValue());
        std::array<AnimationEventOccurrence, 4> drained;
        std::uint32_t count = 99;
        CHECK(instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), drained, count).failure == GraphInstanceFailure::StateInvalid);
        REQUIRE(instance.Abort().HasValue());
        Tick first(instance, 1, 15);
        REQUIRE(first.count == 1);
        CHECK(first.events[0].id.serial == 1);
        CHECK(instance.Evaluate({Id<AnimationTickId>(2), {100}}).failure == GraphInstanceFailure::LimitExceeded);
        CHECK(instance.Publication().tick == tick);
        REQUIRE(instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), drained, count).HasValue());
        REQUIRE(count == 1);
        CHECK(drained[0] == first.events[0]);
        Tick retry(instance, 2, 100);
        REQUIRE(retry.count == 1);
        CHECK(retry.events[0].id.serial == 2);
        CHECK(retry.events[0].id.loopOrdinal == 1);
    }

    TEST_CASE("Reverse notifies use directed half-open intervals and explicit reverse eligibility", "[animation][notifies]") {
        Instance::Prepared fixture;
        auto rejected = Event(3, 75);
        rejected.fireInReverse = false;
        Events(fixture, 0, {Event(1, 50), Event(2, 20, AnimationDeltaTime{40}), rejected});
        auto instance = fixture.Create();
        Tick reverse(instance, 1, 70, {-1'000'000});
        CHECK(Count(reverse, 1, AnimationNotifyPhase::Point) == 1);
        CHECK(Count(reverse, 2, AnimationNotifyPhase::Begin) == 1);
        CHECK(Count(reverse, 2, AnimationNotifyPhase::End) == 0);
        CHECK(Count(reverse, 3, AnimationNotifyPhase::Point) == 0);
        Tick atStart(instance, 2, 10, {-1'000'000});
        CHECK(atStart.count == 0);
        Tick departStart(instance, 3, 1, {-1'000'000});
        REQUIRE(departStart.count == 1);
        CHECK(departStart.events[0].id.phase == AnimationNotifyPhase::End);
        CHECK(departStart.events[0].time.ticks == 20);
    }

    TEST_CASE("Clip notifies retain immutable snapshots and queued payload values across shutdown", "[animation][notifies][lifecycle]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10, AnimationDeltaTime{50})});
        Subscription subscription(fixture.context);
        auto instance = fixture.Create();
        Tick first(instance, 1, 20);
        REQUIRE(first.count == 1);
        const auto retained = first.events[0];
        REQUIRE(instance.Shutdown().HasValue());
        std::array<AnimationEventOccurrence, 4> output;
        std::uint32_t count = 0;
        REQUIRE(instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), output, count).HasValue());
        REQUIRE(count == 1);
        CHECK(output[0] == retained);
        REQUIRE(instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), output, count).HasValue());
        CHECK(count == 0);
        REQUIRE(instance.CopyClipEvents(first.committed.publication.currentPose, output, count).HasValue());
        CHECK(count == 1);
        CHECK(output[0] == retained);
        CHECK(instance.Evaluate({Id<AnimationTickId>(2), {60}}).failure == GraphInstanceFailure::AdmissionRejected);
    }

    TEST_CASE("Clip event backlog fences replacement without dropping owner drain or old duration incarnation",
              "[animation][notifies][reload]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10, AnimationDeltaTime{60})});
        Subscription subscription(fixture.context);
        auto instance = fixture.Create();
        Tick first(instance, 1, 20);
        const auto replacement = fixture.ReplacementContext();
        CHECK(instance.Replace(fixture.Assets(), replacement).HasError());
        CHECK(instance.Publication().currentPose == first.committed.publication.currentPose);
        std::array<AnimationEventOccurrence, 4> output;
        std::uint32_t count = 0;
        REQUIRE(instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), output, count).HasValue());
        REQUIRE(count == 1);
        CHECK(output[0].id.instance == fixture.context.instance);
        REQUIRE(instance.Replace(fixture.Assets(), replacement).HasValue());
        CHECK(instance.CopyClipEvents(first.committed.publication.currentPose, output, count).failure == GraphInstanceFailure::HandleStale);
        Tick next(instance, 1, 20);
        REQUIRE(next.count == 1);
        CHECK(next.events[0].id.instance == replacement.instance);
        CHECK(next.events[0].id.phase == AnimationNotifyPhase::Begin);
        CHECK(next.events[0].windowBeginSerial == 1);
    }

    TEST_CASE("Presentation preview and unknown animation domains cannot acquire authoritative notify queues",
              "[animation][notifies][domain]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10)});
        SECTION("preview samples no gameplay events") {
            fixture.context.evaluationDomain = AnimationEvaluationDomain::EditorPreview;
            auto instance = fixture.Create();
            Tick tick(instance, 1, 50);
            CHECK(tick.count == 0);
        }
        SECTION("presentation samples no gameplay events") {
            fixture.context.evaluationDomain = AnimationEvaluationDomain::Presentation;
            auto instance = fixture.Create();
            Tick tick(instance, 1, 50);
            CHECK(tick.count == 0);
        }
        SECTION("preview subscriptions fail closed") {
            Subscription subscription(fixture.context);
            fixture.context.evaluationDomain = AnimationEvaluationDomain::EditorPreview;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), fixture.context).HasError());
        }
        SECTION("unknown domain fails closed") {
            fixture.context.evaluationDomain = AnimationEvaluationDomain::Count;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), fixture.context).HasError());
        }
    }

    TEST_CASE("Clip notify successful evaluation commit copy and drain never allocate", "[animation][notifies][allocation]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 0), Event(2, 10, AnimationDeltaTime{10})});
        Subscription subscription(fixture.context);
        auto instance = fixture.Create();
        std::array<AnimationEventOccurrence, 32> output;
        std::uint32_t count = 0;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({Id<AnimationTickId>(1), {30}});
        const auto committed = instance.Commit(Id<AnimationTickId>(1));
        const auto copied = instance.CopyClipEvents(committed.publication.currentPose, output, count);
        const auto drained = instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), output, count);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(before == after);
        REQUIRE(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(copied.HasValue());
        CHECK(drained.HasValue());
        CHECK(count == 3);
    }

    TEST_CASE("Notify owner affinity and cold payload budgets fail before state or caller outputs change",
              "[animation][notifies][admission]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10)});
        Subscription subscription(fixture.context);
        SECTION("full retained payload charge") {
            fixture.context.maximumStorageBytes = 1024;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), fixture.context).HasError());
        }
        SECTION("pinned metadata exceeds caller limit") {
            fixture.context.clipLimits.maximumEvents = 0;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), fixture.context).HasError());
        }
        SECTION("unknown subscribed schema") {
            subscription.types[0].schemaRevision = 2;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), fixture.context).HasError());
        }
        SECTION("foreign thread preserves output") {
            auto instance = fixture.Create();
            Tick first(instance, 1, 20);
            std::array<AnimationEventOccurrence, 4> output;
            std::uint32_t count = 99;
            GraphInstanceOutcome outcome;
            std::jthread thread([&] {
                outcome = instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), output, count);
            });
            thread.join();
            CHECK(outcome.failure == GraphInstanceFailure::ThreadViolation);
            CHECK(count == 99);
            REQUIRE(instance.DrainClipEvents(Id<AnimationEventConsumerId>(7), output, count).HasValue());
            CHECK(count == 1);
        }
    }

    TEST_CASE("Held sampling creates no activation point and PingPong turns deliver their endpoint once", "[animation][notifies]") {
        Instance::Prepared fixture;
        // Keep the actual compiler descriptor unchanged by compiling the PingPong publication too.
        auto data = fixture.clips[0]->Data();
        data.descriptor.wrapMode = AnimationWrapMode::PingPong;
        data.eventSchemas = {Schema()};
        data.events = {Event(1, 0), Event(2, 100), Event(3, 20, AnimationDeltaTime{20})};
        auto created = AnimationClipAsset::Create(data, *fixture.skeleton);
        REQUIRE(created.HasValue());
        fixture.compileClips[0] = created.Value();
        fixture.clips[0] = std::make_shared<const AnimationClipAsset>(std::move(created).Value());
        AnimationGraphCompileContext compile;
        compile.skeleton = fixture.skeleton.get();
        compile.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
        compile.clips = fixture.compileClips;
        auto program = AnimationGraphProgram::Compile(Graph::Simple(), compile);
        REQUIRE(program.HasValue());
        fixture.program = std::make_shared<const AnimationGraphProgram>(std::move(program).Value());
        const auto unspecified = AnimationGraphInstance::Create(fixture.Assets(), fixture.context);
        REQUIRE(unspecified.HasError());
        CHECK(unspecified.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        fixture.context.evaluationDomain = AnimationEvaluationDomain::AuthoritativeSimulation;
        auto instance = fixture.Create();
        Tick held(instance, 1, 0);
        CHECK(held.count == 0);
        Tick turn(instance, 2, 150);
        CHECK(Count(turn, 1, AnimationNotifyPhase::Point) == 1);
        CHECK(Count(turn, 2, AnimationNotifyPhase::Point) == 1);
        CHECK(Count(turn, 3, AnimationNotifyPhase::Begin) == 1);
        CHECK(Count(turn, 3, AnimationNotifyPhase::End) == 1);
        Tick reverse(instance, 3, 50);
        CHECK(Count(reverse, 1, AnimationNotifyPhase::Point) == 1);
        CHECK(Count(reverse, 3, AnimationNotifyPhase::Begin) == 1);
        CHECK(Count(reverse, 3, AnimationNotifyPhase::End) == 1);
    }

    TEST_CASE("Event-empty unspecified legacy reload requires explicit component domain before metadata admission",
              "[animation][notifies][reload][domain]") {
        Instance::Prepared original;
        CHECK(original.context.evaluationDomain == AnimationEvaluationDomain::Count);
        auto instance = original.Create();
        Tick old(instance, 1, 20);
        Instance::Prepared replacement(Graph::Simple(), 7);
        Events(replacement, 0, {Event(1, 10)});
        auto context = original.ReplacementContext();
        CHECK(context.evaluationDomain == AnimationEvaluationDomain::Count);
        CHECK(instance.Replace(replacement.Assets(), context).HasError());
        CHECK(instance.Publication().currentPose == old.committed.publication.currentPose);
        std::array<AnimationEventOccurrence, 4> output;
        std::uint32_t count = 99;
        REQUIRE(instance.CopyClipEvents(old.committed.publication.currentPose, output, count).HasValue());
        CHECK(count == 0);
        context.evaluationDomain = AnimationEvaluationDomain::AuthoritativeSimulation;
        REQUIRE(instance.Replace(replacement.Assets(), context).HasValue());
        Tick actual(instance, 1, 20);
        REQUIRE(actual.count == 1);
        CHECK(actual.events[0].id.clipGeneration == Id<AnimationClipGeneration>(7));
    }

}  // namespace Horo::Animation::Test
