#include "AllocationProbe.h"
#include "Horo/Editor/AnimationPreviewSession.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace Animation;

        template <typename T> T Id(const std::uint64_t value) {
            const auto id = T::Create(value);
            REQUIRE(id.HasValue());
            return id.Value();
        }

        template <typename T> T Asset(const std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            const auto id = T::Create(Assets::AssetId::FromBytes(bytes));
            REQUIRE(id.HasValue());
            return id.Value();
        }

        /** @brief Owns a complete production-validated immutable clip and caller-owned pose buffer. */
        struct PreviewFixture final {
            AnimationPreviewSession session;
            std::shared_ptr<const AnimationClipAsset> clip;
            std::array<Math::Transform, 1> output;
            AnimationClipSampleContext context;

            explicit PreviewFixture(const AnimationWrapMode mode = AnimationWrapMode::Loop) {
                SkeletonAssetData skeletonData{.skeleton = Asset<SkeletonId>(1), .joints = {{.id = Id<JointId>(1), .name = "root"}}};
                auto skeleton = SkeletonAsset::Create(std::move(skeletonData));
                REQUIRE(skeleton.HasValue());
                AnimationClipData data{.descriptor = {.id = Asset<AnimationClipId>(2),
                                                      .generation = Id<AnimationClipGeneration>(1),
                                                      .skeleton = Asset<SkeletonId>(1),
                                                      .skeletonGeneration = Id<SkeletonAssetGeneration>(1),
                                                      .duration = {100},
                                                      .wrapMode = mode},
                                       .tracks = {{.joint = Id<JointId>(1),
                                                   .keys = {{.time = {0}}, {.time = {100}, .transform = {.translation = {10, 0, 0}}}}}}};
                auto created = AnimationClipAsset::Create(std::move(data), skeleton.Value());
                REQUIRE(created.HasValue());
                clip = std::make_shared<const AnimationClipAsset>(std::move(created).Value());
                context = {.skeleton = Asset<SkeletonId>(1),
                           .skeletonGeneration = Id<SkeletonAssetGeneration>(1),
                           .clipGeneration = Id<AnimationClipGeneration>(1),
                           .outputPose = output};
                REQUIRE(session.Bind(1, 7, clip).HasValue());
            }

            std::uint64_t Generation() const {
                return session.State().generation;
            }

            AnimationClipOutcome<AnimationClipSample> Advance(const std::int64_t ticks) {
                return session.Advance(Generation(), {ticks}, context);
            }
        };
    }  // namespace

    TEST_CASE("Animation preview transport uses canonical wrap and reverse sampling", "[animation][editor][preview]") {
        PreviewFixture fixture;
        REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
        const auto forward = fixture.Advance(125);
        REQUIRE(forward.HasValue());
        CHECK(forward.Value().time.ticks == 25);
        CHECK(fixture.output.front().translation.x == 2.5F);
        const auto reverse = fixture.Advance(-50);
        REQUIRE(reverse.HasValue());
        CHECK(reverse.Value().time.ticks == 75);
        CHECK(fixture.output.front().translation.x == 7.5F);
        REQUIRE(fixture.session.SetPlaying(fixture.Generation(), false).HasValue());
        REQUIRE(fixture.Advance(500).HasValue());
        CHECK(fixture.session.State().playhead.ticks == 75);
    }

    TEST_CASE("Animation preview seek is silent paused and admits the exact loop endpoint", "[animation][editor][preview]") {
        PreviewFixture fixture;
        REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
        REQUIRE(fixture.session.Seek(fixture.Generation(), {100}, fixture.context).HasValue());
        CHECK_FALSE(fixture.session.State().playing);
        CHECK(fixture.output.front().translation.x == 10.0F);
        REQUIRE(fixture.Advance(10).HasValue());
        CHECK(fixture.session.State().playhead.ticks == 100);
        REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
        REQUIRE(fixture.Advance(25).HasValue());
        CHECK(fixture.session.State().playhead.ticks == 25);
        CHECK(fixture.output.front().translation.x == 2.5F);
    }

    TEST_CASE("Animation preview terminal and ping pong state follow production traversal", "[animation][editor][preview]") {
        SECTION("once pauses and explicit seek permits replay") {
            PreviewFixture fixture{AnimationWrapMode::Once};
            REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
            REQUIRE(fixture.Advance(150).HasValue());
            CHECK_FALSE(fixture.session.State().playing);
            CHECK(fixture.session.State().playhead.ticks == 100);
            REQUIRE(fixture.session.Seek(fixture.Generation(), {20}, fixture.context).HasValue());
            REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
            REQUIRE(fixture.Advance(30).HasValue());
            CHECK(fixture.session.State().playhead.ticks == 50);
        }
        SECTION("ping pong pause retains reflected sample") {
            PreviewFixture fixture{AnimationWrapMode::PingPong};
            REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
            REQUIRE(fixture.Advance(125).HasValue());
            CHECK(fixture.session.State().playhead.ticks == 75);
            REQUIRE(fixture.Advance(0).HasValue());
            REQUIRE(fixture.session.SetPlaying(fixture.Generation(), false).HasValue());
            REQUIRE(fixture.Advance(50).HasValue());
            CHECK(fixture.session.State().playhead.ticks == 75);
            REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
            REQUIRE(fixture.Advance(25).HasValue());
            CHECK(fixture.session.State().playhead.ticks == 50);
        }
    }

    TEST_CASE("Animation preview failures preserve pose cursor and playback atomically", "[animation][editor][preview]") {
        PreviewFixture fixture;
        REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
        REQUIRE(fixture.Advance(25).HasValue());
        const auto before = fixture.session.State();
        const auto pose = fixture.output;
        SECTION("stale clip binding") {
            fixture.context.clipGeneration = Id<AnimationClipGeneration>(2);
            CHECK(fixture.Advance(25).ErrorValue().failure == AnimationClipFailure::BindingStale);
        }
        SECTION("cancelled sample") {
            fixture.context.admission = AnimationClipAdmissionState::CancellationRequested;
            CHECK(fixture.Advance(25).ErrorValue().failure == AnimationClipFailure::Cancelled);
        }
        SECTION("wrong output size") {
            fixture.context.outputPose = {};
            CHECK(fixture.Advance(25).HasError());
        }
        SECTION("excessive traversal") {
            CHECK(fixture.Advance(std::numeric_limits<std::int64_t>::max()).HasError());
        }
        SECTION("out of range seek") {
            CHECK(fixture.session.Seek(fixture.Generation(), {-1}, fixture.context).HasError());
            CHECK(fixture.session.Seek(fixture.Generation(), {101}, fixture.context).HasError());
        }
        CHECK(fixture.session.State() == before);
        CHECK(fixture.output == pose);
    }

    TEST_CASE("Animation preview replacement fences delayed commands and close releases publication", "[animation][editor][preview]") {
        PreviewFixture fixture;
        const auto generation = fixture.Generation();
        const auto before = fixture.session.State();
        CHECK(fixture.session.Bind(generation, 0, fixture.clip).HasError());
        CHECK(fixture.session.Bind(generation, 8, {}).HasError());
        CHECK(fixture.session.State() == before);
        REQUIRE(fixture.session.Bind(generation, 8, fixture.clip).HasValue());
        CHECK(fixture.session.State().sourceRevision == 8);
        CHECK(fixture.session.SetPlaying(generation, true).ErrorValue().failure == AnimationClipFailure::BindingStale);
        CHECK(fixture.session.Seek(generation, {10}, fixture.context).HasError());
        CHECK(fixture.session.Advance(generation, {10}, fixture.context).HasError());
        CHECK(fixture.session.Bind(generation, 9, fixture.clip).HasError());
        std::weak_ptr<const AnimationClipAsset> retained = fixture.clip;
        fixture.clip.reset();
        CHECK_FALSE(retained.expired());
        fixture.session.Close();
        fixture.session.Close();
        CHECK(retained.expired());
        CHECK(fixture.session.State().closed);
        CHECK_FALSE(fixture.session.State().playing);
        CHECK(fixture.session.Bind(fixture.Generation(), 9, {}).HasError());
        CHECK(fixture.Advance(1).HasError());
    }

    TEST_CASE("Animation preview sessions are independent and hot operations allocate nothing", "[animation][editor][preview]") {
        PreviewFixture fixture;
        AnimationPreviewSession other;
        REQUIRE(other.Bind(1, 7, fixture.clip).HasValue());
        REQUIRE(fixture.session.SetPlaying(fixture.Generation(), true).HasValue());
        bool succeeded = true;
        Tests::AllocationProbe::Measurement allocation;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            for (unsigned index = 0; index < 100; ++index)
                succeeded = fixture.Advance(1).HasValue() && succeeded;
            succeeded = fixture.session.Seek(fixture.Generation(), {50}, fixture.context).HasValue() && succeeded;
            fixture.context.admission = AnimationClipAdmissionState::CancellationRequested;
            succeeded = fixture.Advance(1).HasError() && succeeded;
            allocation = measurement.Snapshot();
        }
        CHECK(succeeded);
        CHECK(allocation.requests == 0);
        CHECK(other.State().playhead.ticks == 0);
        CHECK_FALSE(other.State().playing);
    }
}  // namespace Horo::Editor
