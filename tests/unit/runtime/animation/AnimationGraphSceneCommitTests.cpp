#include "AnimationGraphInstanceTestFixtures.h"
#include "AnimationGraphSceneCommitAccess.h"

namespace Horo::Animation {
    using Test::Id;
    using Test::Instance::Prepared;

    TEST_CASE("Scene graph preparation stages without publishing and ordinary Commit cannot bypass its owner",
              "[animation][graph][scene-commit]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {10}}).HasValue());
        REQUIRE(Detail::GraphSceneCommitAccess::Prepare(instance, tick).HasValue());
        CHECK_FALSE(instance.Publication().currentPose.IsValid());
        CHECK(Detail::GraphSceneCommitAccess::Prepare(instance, tick).failure == GraphInstanceFailure::StateInvalid);
        CHECK(instance.Commit(tick).failure == GraphInstanceFailure::StateInvalid);
        const auto publication = Detail::GraphSceneCommitAccess::Publish(instance);
        CHECK(publication.tick == tick);
        CHECK(publication.currentPose.IsValid());
        CHECK(instance.Publication().currentPose == publication.currentPose);
    }

    TEST_CASE("Scene graph cancellation before preparation retains the previous publication", "[animation][graph][scene-commit]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto first = Id<AnimationTickId>(1);
        const auto next = Id<AnimationTickId>(2);
        REQUIRE(instance.Evaluate({first, {10}}).HasValue());
        const auto committed = instance.Commit(first);
        REQUIRE(committed.HasValue());
        CancellationSource cancellation;
        REQUIRE(instance.Evaluate({next, {10}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(Detail::GraphSceneCommitAccess::Prepare(instance, next).failure == GraphInstanceFailure::Cancelled);
        CHECK(instance.Publication().tick == first);
        CHECK(instance.Publication().currentPose == committed.publication.currentPose);
        REQUIRE(instance.Evaluate({next, {10}}).HasValue());
        CHECK(instance.Commit(next).HasValue());
    }

    TEST_CASE("Scene graph armed publication has no second cancellation gate or hot allocation", "[animation][graph][scene-commit]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        CancellationSource cancellation;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({tick, {10}, {}, {}, cancellation.Token()});
        const auto prepared = Detail::GraphSceneCommitAccess::Prepare(instance, tick);
        cancellation.RequestCancellation();
        const auto publication = Detail::GraphSceneCommitAccess::Publish(instance);
        const auto after = HoroAnimationTestAllocationCount();
        REQUIRE(evaluated.HasValue());
        REQUIRE(prepared.HasValue());
        CHECK(after == before);
        CHECK(publication.tick == tick);
        CHECK(publication.currentPose.IsValid());
    }

    TEST_CASE("Scene graph abort clears an armed candidate and preserves its real cursor", "[animation][graph][scene-commit]") {
        Prepared fixture;
        auto instance = fixture.Create();
        const auto tick = Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {25}}).HasValue());
        CHECK(Detail::GraphSceneCommitAccess::Prepare(instance, Id<AnimationTickId>(2)).failure == GraphInstanceFailure::TickStale);
        REQUIRE(Detail::GraphSceneCommitAccess::Prepare(instance, tick).HasValue());
        REQUIRE(instance.Abort().HasValue());
        CHECK_FALSE(instance.Publication().currentPose.IsValid());
        REQUIRE(instance.Evaluate({tick, {10}}).HasValue());
        REQUIRE(Detail::GraphSceneCommitAccess::Prepare(instance, tick).HasValue());
        const auto publication = Detail::GraphSceneCommitAccess::Publish(instance);
        std::array<Math::Transform, 1> pose;
        REQUIRE(instance.CopyPose(publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 10.0F);
    }
}  // namespace Horo::Animation
