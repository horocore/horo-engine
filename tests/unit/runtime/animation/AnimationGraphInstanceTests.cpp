#include "AnimationGraphInstanceTestFixtures.h"

#include <array>
#include <cmath>
#include <limits>
#include <thread>

namespace Horo::Animation {
    using Test::Id;
    using Test::Instance::Prepared;

    TEST_CASE("Animation graph instance stages cursors and fractional rate until atomic commit", "[animation][graph][instance]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto tick1 = Id<AnimationTickId>(1);
        const auto tick2 = Id<AnimationTickId>(2);
        const auto tick3 = Id<AnimationTickId>(3);
        std::array<Math::Transform, 1> pose;
        const auto before = HoroAnimationTestAllocationCount();
        const auto staged = instance.Evaluate({tick1, {1}, {500'000}});
        const auto unpublished = instance.Publication();
        const auto committed = instance.Commit(tick1);
        const auto firstRead = instance.CopyPose(committed.publication.currentPose, pose);
        const float first = pose[0].translation.x;
        const auto discarded = instance.Evaluate({tick2, {1}, {500'000}});
        const auto aborted = instance.Abort();
        const auto next = instance.Evaluate({tick3, {1}, {500'000}});
        const auto secondCommit = instance.Commit(tick3);
        const auto secondRead = instance.CopyPose(secondCommit.publication.currentPose, pose);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        REQUIRE(staged.HasValue());
        CHECK_FALSE(unpublished.currentPose.IsValid());
        REQUIRE(committed.HasValue());
        REQUIRE(firstRead.HasValue());
        CHECK(first == 0.0F);
        CHECK(discarded.HasValue());
        CHECK(aborted.HasValue());
        CHECK(next.HasValue());
        REQUIRE(secondCommit.HasValue());
        CHECK(secondRead.HasValue());
        CHECK(pose[0].translation.x == 1.0F);
        CHECK(secondCommit.publication.previousPose == committed.publication.currentPose);
    }

    TEST_CASE("Animation graph instance copies typed parameters and blends actual immutable clips", "[animation][graph][instance]") {
        Prepared fixture(Test::Instance::Blended());
        auto instance = fixture.Create();
        std::array<GraphParameterInput, 1> parameters{{{Id<GraphParameterId>(1), 0.25F}}};
        const auto tick = Id<AnimationTickId>(1);
        std::array<Math::Transform, 1> pose;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({tick, {25}, {}, parameters});
        parameters[0].value = 1.0F;
        const auto committed = instance.Commit(tick);
        const auto copied = instance.CopyPose(committed.publication.currentPose, pose);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        REQUIRE(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(copied.HasValue());
        CHECK(pose[0].translation.x == 50.0F);
        CHECK(pose[0].rotation == Math::Quaternion::Identity());
    }

    TEST_CASE("Animation graph instance failure cancellation and shutdown preserve published poses without allocation",
              "[animation][graph][instance]") {
        Prepared fixture;
        fixture.context.clipLimits.maximumBoundaryCrossings = 0;
        auto instance = fixture.Create();
        const auto tick1 = Id<AnimationTickId>(1);
        const auto tick2 = Id<AnimationTickId>(2);
        const auto tick3 = Id<AnimationTickId>(3);
        CancellationSource cancellation;
        std::array<Math::Transform, 1> pose;
        const auto before = HoroAnimationTestAllocationCount();
        const auto initial = instance.Evaluate({tick1, {10}});
        const auto first = instance.Commit(tick1);
        const auto limited = instance.Evaluate({tick2, {101}});
        const auto oldRead = instance.CopyPose(first.publication.currentPose, pose);
        const float oldPosition = pose[0].translation.x;
        const auto pending = instance.Evaluate({tick3, {5}, {}, {}, cancellation.Token()});
        cancellation.RequestCancellation();
        const auto cancelled = instance.Commit(tick3);
        const auto stale = instance.Evaluate({tick1, {1}});
        const auto closed = instance.Shutdown();
        const auto rejected = instance.Evaluate({tick3, {1}});
        const auto retained = instance.CopyPose(first.publication.currentPose, pose);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(initial.HasValue());
        REQUIRE(first.HasValue());
        CHECK(limited.failure == GraphInstanceFailure::ClipFailure);
        CHECK(limited.clipFailure.failure == AnimationClipFailure::LimitExceeded);
        CHECK(oldRead.HasValue());
        CHECK(oldPosition == 10.0F);
        CHECK(pending.HasValue());
        CHECK(cancelled.failure == GraphInstanceFailure::Cancelled);
        CHECK(stale.failure == GraphInstanceFailure::TickStale);
        CHECK(closed.HasValue());
        CHECK(rejected.failure == GraphInstanceFailure::AdmissionRejected);
        CHECK(retained.HasValue());
        CHECK(pose[0].translation.x == 10.0F);
        CHECK(instance.Publication().tick == tick1);
    }

    TEST_CASE("Animation graph instance admits complete storage and exact dependency publications", "[animation][graph][instance]") {
        Prepared fixture;
        SECTION("complete storage capacity") {
            auto context = fixture.context;
            context.maximumStorageBytes = fixture.program->Memory().poseTransforms * sizeof(Math::Transform);
            const auto created = AnimationGraphInstance::Create(fixture.Assets(), context);
            REQUIRE(created.HasError());
            CHECK(created.ErrorValue().code.Value() == AnimationErrors::GraphLimitExceeded.code.Value());
        }
        SECTION("skeleton generation") {
            auto assets = fixture.Assets();
            assets.skeletonGeneration = Id<SkeletonAssetGeneration>(6);
            CHECK(AnimationGraphInstance::Create(assets, fixture.context).HasError());
        }
        SECTION("clip generation") {
            Prepared replacement(Test::Graph::Simple(), 7);
            auto assets = fixture.Assets();
            assets.clips = replacement.clips;
            CHECK(AnimationGraphInstance::Create(assets, fixture.context).HasError());
        }
        SECTION("complete per-tick sample work") {
            auto context = fixture.context;
            context.maximumJointOperations = 0;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasError());
            context.maximumJointOperations = 4;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasValue());
            context.maximumJointOperations = 3;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasError());
        }
        SECTION("clip key policy") {
            auto context = fixture.context;
            context.clipLimits.maximumKeysPerTrack = 1;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasError());
        }
        SECTION("instruction ceiling") {
            auto context = fixture.context;
            context.maximumInstructions = 1;
            CHECK(AnimationGraphInstance::Create(fixture.Assets(), context).HasError());
        }
    }

    TEST_CASE("Animation graph instance explicit replacement retires handles only after preparation succeeds",
              "[animation][graph][instance]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {20}}).HasValue());
        const auto committed = instance.Commit(tick);
        REQUIRE(committed.HasValue());
        auto context = fixture.context;
        context.instance.slot.generation = 2;
        auto invalid = fixture.Assets();
        invalid.skeletonGeneration = Id<SkeletonAssetGeneration>(8);
        CHECK(instance.Replace(invalid, context).HasError());
        std::array<Math::Transform, 1> pose;
        CHECK(instance.CopyPose(committed.publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 20.0F);
        Prepared replacement(Test::Graph::Simple(), 7);
        REQUIRE(instance.Replace(replacement.Assets(), context).HasValue());
        CHECK(instance.CopyPose(committed.publication.currentPose, pose).failure == GraphInstanceFailure::HandleStale);
        CHECK_FALSE(instance.Publication().currentPose.IsValid());
        CHECK(instance.Replace(fixture.Assets(), context).HasError());
    }

    TEST_CASE("Animation graph instance rejects wrong-thread execution and recycles exact pose generations",
              "[animation][graph][instance]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto tick1 = Id<AnimationTickId>(1);
        GraphInstanceOutcome wrongThread;
        std::jthread worker([&instance, &wrongThread, tick1] {
            wrongThread = instance.Evaluate({tick1, {1}});
        });
        worker.join();
        CHECK(wrongThread.failure == GraphInstanceFailure::ThreadViolation);
        std::array<PoseHandle, 3> handles;
        for (std::uint64_t tick = 1; tick <= 3; ++tick) {
            const auto id = Id<AnimationTickId>(tick);
            REQUIRE(instance.Evaluate({id, {1}}).HasValue());
            const auto committed = instance.Commit(id);
            REQUIRE(committed.HasValue());
            handles[tick - 1] = committed.publication.currentPose;
        }
        std::array<Math::Transform, 1> pose;
        CHECK(instance.CopyPose(handles[0], pose).failure == GraphInstanceFailure::HandleStale);
        CHECK(instance.CopyPose(handles[1], pose).HasValue());
        CHECK(pose[0].translation.x == 2.0F);
        CHECK(instance.CopyPose(handles[2], pose).HasValue());
        CHECK(pose[0].translation.x == 3.0F);
    }

    TEST_CASE("Animation graph instance executes compiler call aliases and repeated independent players", "[animation][graph][instance]") {
        for (auto graph : {Test::Graph::Subgraphs(), Test::Graph::RepeatedCalls()}) {
            Prepared fixture(std::move(graph));
            auto instance = fixture.Create();
            const auto tick = Id<AnimationTickId>(1);
            std::array<Math::Transform, 1> pose;
            const auto before = HoroAnimationTestAllocationCount();
            const auto evaluated = instance.Evaluate({tick, {37}});
            const auto committed = instance.Commit(tick);
            const auto copied = instance.CopyPose(committed.publication.currentPose, pose);
            const auto after = HoroAnimationTestAllocationCount();
            CHECK(after == before);
            REQUIRE(evaluated.HasValue());
            REQUIRE(committed.HasValue());
            CHECK(copied.HasValue());
            CHECK(pose[0].translation.x == 37.0F);
        }
    }

    TEST_CASE("Animation graph instance rejects malformed input and exact overflow before state publication",
              "[animation][graph][instance]") {
        Prepared fixture(Test::Instance::Blended());
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        std::array<GraphParameterInput, 1> wrong{{{Id<GraphParameterId>(1), true}}};
        std::array<GraphParameterInput, 1> nonfinite{{{Id<GraphParameterId>(1), std::numeric_limits<float>::infinity()}}};
        const auto before = HoroAnimationTestAllocationCount();
        const auto mistyped = instance.Evaluate({tick, {1}, {}, wrong});
        const auto invalid = instance.Evaluate({tick, {1}, {}, nonfinite});
        const auto overflow = instance.Evaluate({tick, {std::numeric_limits<std::int64_t>::max()}, {16'000'000}});
        const auto negative = instance.Evaluate({tick, {-1}});
        const auto resumed = instance.Evaluate({tick, {20}});
        const auto duplicate = instance.Evaluate({tick, {1}});
        const auto mismatchedCommit = instance.Commit({});
        const auto committed = instance.Commit(tick);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(mistyped.failure == GraphInstanceFailure::TypeMismatch);
        CHECK(invalid.failure == GraphInstanceFailure::Malformed);
        CHECK(overflow.failure == GraphInstanceFailure::ClipFailure);
        CHECK(overflow.clipFailure.failure == AnimationClipFailure::TimeOverflow);
        CHECK(negative.failure == GraphInstanceFailure::Malformed);
        CHECK(resumed.HasValue());
        CHECK(duplicate.failure == GraphInstanceFailure::StateInvalid);
        CHECK(mismatchedCommit.failure == GraphInstanceFailure::TickStale);
        REQUIRE(committed.HasValue());
        std::array<Math::Transform, 1> pose;
        CHECK(instance.CopyPose(committed.publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 70.0F);
        const auto projected = DescribeGraphInstanceFailure(overflow);
        CHECK(projected.code.Value() == AnimationErrors::ClipTimeOverflow.code.Value());
    }

    TEST_CASE("Animation graph instance preserves exact rate histories across partition hold and reverse", "[animation][graph][instance]") {
        Prepared fixture;
        auto partitioned = fixture.Create();
        auto combined = fixture.Create();
        for (std::uint64_t tick = 1; tick <= 3; ++tick) {
            const auto id = Id<AnimationTickId>(tick);
            REQUIRE(partitioned.Evaluate({id, {11}, {333'333}}).HasValue());
            REQUIRE(partitioned.Commit(id).HasValue());
        }
        const auto tick = Id<AnimationTickId>(3);
        REQUIRE(combined.Evaluate({tick, {33}, {333'333}}).HasValue());
        REQUIRE(combined.Commit(tick).HasValue());
        std::array<Math::Transform, 1> first, second;
        REQUIRE(partitioned.CopyPose(partitioned.Publication().currentPose, first).HasValue());
        REQUIRE(combined.CopyPose(combined.Publication().currentPose, second).HasValue());
        CHECK(first == second);
        CHECK(first[0].translation.x == 10.0F);
        const auto holdTick = Id<AnimationTickId>(4);
        REQUIRE(partitioned.Evaluate({holdTick, {1000}, {0}}).HasValue());
        REQUIRE(partitioned.Commit(holdTick).HasValue());
        REQUIRE(partitioned.CopyPose(partitioned.Publication().currentPose, first).HasValue());
        CHECK(first == second);
        const auto reverseTick = Id<AnimationTickId>(5);
        REQUIRE(partitioned.Evaluate({reverseTick, {20}, {-1'000'000}}).HasValue());
        REQUIRE(partitioned.Commit(reverseTick).HasValue());
        REQUIRE(partitioned.CopyPose(partitioned.Publication().currentPose, first).HasValue());
        CHECK(first[0].translation.x == 91.0F);
    }

    TEST_CASE("Animation prepared clip failures allocate nothing and retain legacy diagnostics", "[animation][clip][prepared]") {
        Prepared fixture;
        const auto &clip = *fixture.clips.front();
        std::array<Math::Transform, 1> pose;
        AnimationClipSampleContext context{.skeleton = clip.Data().descriptor.skeleton,
                                           .skeletonGeneration = clip.Data().descriptor.skeletonGeneration,
                                           .clipGeneration = clip.Data().descriptor.generation,
                                           .outputPose = pose};
        AnimationClipLimits limits;
        limits.maximumBoundaryCrossings = 0;
        const auto before = HoroAnimationTestAllocationCount();
        const auto traversal = clip.TraversePrepared({}, {101}, limits);
        const auto malformed = clip.SamplePrepared({101}, context);
        const auto cancelled = clip.SamplePrepared({0}, {.admission = AnimationClipAdmissionState::CancellationRequested});
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(traversal.ErrorValue().failure == AnimationClipFailure::LimitExceeded);
        CHECK(malformed.ErrorValue().failure == AnimationClipFailure::Malformed);
        CHECK(cancelled.ErrorValue().failure == AnimationClipFailure::Cancelled);
        const auto legacy = clip.Traverse({}, {101}, limits);
        REQUIRE(legacy.HasError());
        const auto projected = DescribeAnimationClipFailure(traversal.ErrorValue());
        CHECK(projected.code.Value() == legacy.ErrorValue().code.Value());
        CHECK(projected.domain.Value() == legacy.ErrorValue().domain.Value());
        CHECK(projected.message == legacy.ErrorValue().message);
    }

    TEST_CASE("Animation graph instance snapshots all compiled scalar types and rejects duplicate overrides",
              "[animation][graph][instance]") {
        // Scalar declarations are captured even when unused; executable readers must reach the pose sink.
        Prepared fixture(Test::Instance::AllScalarTypes());
        AnimationGraphCompileContext compile;
        compile.skeleton = fixture.skeleton.get();
        compile.skeletonGeneration = Id<SkeletonAssetGeneration>(5);
        compile.clips = fixture.compileClips;
        const auto invalidProgram = AnimationGraphProgram::Compile(Test::Instance::AllScalarTypes(true), compile);
        REQUIRE(invalidProgram.HasError());
        CHECK(invalidProgram.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        REQUIRE(fixture.program->Parameters().size() == 4);
        CHECK(fixture.program->Parameters()[1].type == GraphValueType::Boolean);
        CHECK(fixture.program->Parameters()[2].type == GraphValueType::Integer);
        CHECK(fixture.program->Parameters()[3].type == GraphValueType::Trigger);
        auto instance = fixture.Create();
        std::array<GraphParameterInput, 4> parameters{{{Id<GraphParameterId>(4), true},
                                                       {Id<GraphParameterId>(3), std::int32_t{7}},
                                                       {Id<GraphParameterId>(2), true},
                                                       {Id<GraphParameterId>(1), 0.0F}}};
        std::array<GraphParameterInput, 2> duplicate{{parameters[0], parameters[0]}};
        std::array<GraphParameterInput, 4> mistyped{{{Id<GraphParameterId>(1), true},
                                                     {Id<GraphParameterId>(2), std::int32_t{7}},
                                                     {Id<GraphParameterId>(3), true},
                                                     {Id<GraphParameterId>(4), 1.0F}}};
        std::array<GraphInstanceOutcome, 4> typeRejections;
        const auto tick = Id<AnimationTickId>(1);
        const auto before = HoroAnimationTestAllocationCount();
        const auto rejected = instance.Evaluate({tick, {10}, {}, duplicate});
        for (std::size_t index = 0; index < mistyped.size(); ++index)
            typeRejections[index] = instance.Evaluate({tick, {10}, {}, std::span{mistyped}.subspan(index, 1)});
        const auto evaluated = instance.Evaluate({tick, {10}, {}, parameters});
        const auto committed = instance.Commit(tick);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(rejected.failure == GraphInstanceFailure::Malformed);
        for (const auto &rejection : typeRejections)
            CHECK(rejection.failure == GraphInstanceFailure::TypeMismatch);
        CHECK(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        std::array<Math::Transform, 1> pose;
        CHECK(instance.CopyPose(committed.publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 10.0F);
    }

}  // namespace Horo::Animation
