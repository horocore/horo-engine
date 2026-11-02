#include "AnimationTestFixtures.h"
#include "Horo/Animation/AnimationClip.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {
    std::atomic<std::size_t> ClipTestAllocations{};
}

std::size_t HoroAnimationTestAllocationCount() noexcept {
    return ClipTestAllocations.load(std::memory_order_relaxed);
}

void *operator new(const std::size_t size) {
    ClipTestAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) {
    ClipTestAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

namespace Horo::Animation {
    namespace {
        using Test::Asset;
        using Test::Id;

        constexpr std::int64_t Duration = 100;

        SkeletonAsset ClipSkeleton() {
            SkeletonAssetData data{.skeleton = Asset<SkeletonId>(41),
                                   .joints = {{.id = Id<JointId>(10), .name = "root"},
                                              {.id = Id<JointId>(20), .parent = Id<JointId>(10), .name = "spine"},
                                              {.id = Id<JointId>(30), .parent = Id<JointId>(20), .name = "head"}}};
            data.joints[2].referenceLocalTransform.translation.y = 3.0F;
            data.joints[2].inverseBindMatrix = Math::TranslationMatrix({0.0F, -3.0F, 0.0F});
            auto created = SkeletonAsset::Create(std::move(data));
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        AnimationTransformKey Key(const std::int64_t time, const float x,
                                  const AnimationInterpolation interpolation = AnimationInterpolation::Linear) {
            Math::Transform transform{};
            transform.translation.x = x;
            return {.time = {time}, .transform = transform, .interpolation = interpolation};
        }

        AnimationClipData ClipData(const AnimationWrapMode wrapMode = AnimationWrapMode::Loop,
                                   const AnimationClipKind kind = AnimationClipKind::Absolute) {
            AnimationClipDescriptor descriptor{.id = Asset<AnimationClipId>(42),
                                               .generation = Id<AnimationClipGeneration>(43),
                                               .skeleton = Asset<SkeletonId>(41),
                                               .skeletonGeneration = Id<SkeletonAssetGeneration>(44),
                                               .duration = {Duration},
                                               .sampleRate = {30, 1},
                                               .wrapMode = wrapMode,
                                               .kind = kind};
            if (kind == AnimationClipKind::Additive)
                descriptor.referencePose =
                    AnimationReferencePoseBinding{Id<AnimationReferencePoseId>(45), Id<AnimationReferencePoseGeneration>(46)};
            return {.descriptor = descriptor, .tracks = {{.joint = Id<JointId>(20), .keys = {Key(Duration, 10.0F), Key(0, 0.0F)}}}};
        }

        AnimationClipAsset Clip(const SkeletonAsset &skeleton, const AnimationWrapMode wrapMode = AnimationWrapMode::Loop,
                                const AnimationClipKind kind = AnimationClipKind::Absolute) {
            auto created = AnimationClipAsset::Create(ClipData(wrapMode, kind), skeleton);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        AnimationClipSampleContext SampleContext(const std::span<Math::Transform> output) {
            return {.skeleton = Asset<SkeletonId>(41),
                    .skeletonGeneration = Id<SkeletonAssetGeneration>(44),
                    .clipGeneration = Id<AnimationClipGeneration>(43),
                    .outputPose = output};
        }

        AnimationClipSampleContext AdditiveSampleContext(
            const std::span<Math::Transform> output, const std::span<const Math::Transform> base,
            const std::span<const Math::Transform> reference,
            const AnimationReferencePoseGeneration referenceGeneration = Id<AnimationReferencePoseGeneration>(46)) {
            AnimationClipSampleContext context = SampleContext(output);
            context.referencePose = AnimationReferencePoseBinding{Id<AnimationReferencePoseId>(45), referenceGeneration};
            context.basePose = base;
            context.referenceLocalPose = reference;
            return context;
        }

        template <typename Value> void RequireClipError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE_FALSE(result.HasValue());
            const Error &error = result.ErrorValue();
            CHECK(error.domain.Value() == "horo.animation");
            CHECK(error.code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Clip creation canonicalizes track and key order into detached immutable data", "[unit][animation][clip][determinism]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto data = ClipData();
        data.tracks.push_back({.joint = Id<JointId>(10), .keys = {Key(80, 8.0F), Key(20, 2.0F)}});
        auto created = AnimationClipAsset::Create(data, skeleton);
        REQUIRE(created.HasValue());
        data.tracks.front().keys.front().transform.translation.x = 99.0F;
        const auto &canonical = created.Value().Data();
        REQUIRE(canonical.tracks.size() == 2);
        CHECK(canonical.tracks[0].joint == Id<JointId>(10));
        CHECK(canonical.tracks[0].keys[0].time == AnimationTime{20});
        CHECK(canonical.tracks[1].keys[0].time == AnimationTime{0});
        CHECK(canonical.tracks[1].keys[0].transform.translation.x == 0.0F);
    }

    TEST_CASE("Absolute clip sampling preserves untracked reference joints and interpolates tracked transforms",
              "[unit][animation][clip][sample]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton);
        std::vector<Math::Transform> output(3);
        const auto sampled = clip.Sample({50}, SampleContext(output));
        REQUIRE(sampled.HasValue());
        CHECK(sampled.Value().sampledTracks == 1);
        CHECK_FALSE(sampled.Value().additive);
        CHECK(output[1].translation.x == 5.0F);
        CHECK(output[2].translation.y == 3.0F);
    }

    TEST_CASE("A reference-pose-only clip writes a complete deterministic pose", "[unit][animation][clip][sample]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto data = ClipData();
        data.tracks.clear();
        auto created = AnimationClipAsset::Create(std::move(data), skeleton);
        REQUIRE(created.HasValue());
        std::array<Math::Transform, 3> output{};
        const auto sampled = created.Value().Sample({50}, SampleContext(output));
        REQUIRE(sampled.HasValue());
        CHECK(sampled.Value().sampledTracks == 0);
        CHECK(output[2].translation.y == 3.0F);
    }

    TEST_CASE("Step linear and cubic Hermite interpolation are deterministic and rotations use shortest path",
              "[unit][animation][clip][interpolation]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto data = ClipData();
        data.tracks[0].keys = {Key(0, 2.0F, AnimationInterpolation::Step), Key(40, 6.0F), Key(100, 10.0F)};
        data.tracks[0].keys[0].transform.rotation = {0.0F, 0.0F, 0.0F, 2.0F};
        data.tracks[0].keys[1].transform.rotation = {0.0F, 0.0F, 0.0F, -3.0F};
        auto created = AnimationClipAsset::Create(std::move(data), skeleton);
        REQUIRE(created.HasValue());
        std::vector<Math::Transform> output(3);
        REQUIRE(created.Value().Sample({20}, SampleContext(output)).HasValue());
        CHECK(output[1].translation.x == 2.0F);
        CHECK(output[1].rotation.w == 1.0F);

        auto smoothData = ClipData();
        smoothData.tracks[0].keys = {Key(0, 0.0F, AnimationInterpolation::CubicHermite), Key(100, 8.0F)};
        auto smooth = AnimationClipAsset::Create(std::move(smoothData), skeleton);
        REQUIRE(smooth.HasValue());
        REQUIRE(smooth.Value().Sample({25}, SampleContext(output)).HasValue());
        CHECK(output[1].translation.x == 1.25F);
    }

    TEST_CASE("Additive sampling applies an exact reference-relative delta to the current base pose", "[unit][animation][clip][additive]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto data = ClipData(AnimationWrapMode::Loop, AnimationClipKind::Additive);
        data.tracks[0].keys = {Key(0, 2.0F), Key(100, 6.0F)};
        const AnimationClipAsset clip = [&] {
            auto result = AnimationClipAsset::Create(std::move(data), skeleton);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }();
        std::vector<Math::Transform> base(3);
        std::vector<Math::Transform> reference(3);
        std::vector<Math::Transform> output(3);
        base[1].translation.x = 10.0F;
        reference[1].translation.x = 2.0F;
        const auto context = AdditiveSampleContext(output, base, reference);
        const auto sampled = clip.Sample({50}, context);
        REQUIRE(sampled.HasValue());
        CHECK(sampled.Value().additive);
        CHECK(output[1].translation.x == 12.0F);
    }

    TEST_CASE("Once traversal terminates atomically at either directed boundary", "[unit][animation][clip][wrap]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton, AnimationWrapMode::Once);
        const auto forward = clip.Traverse({{90}, false}, {20});
        REQUIRE(forward.HasValue());
        CHECK(forward.Value().cursor == AnimationClipCursor{{100}, true});
        CHECK(forward.Value().sampleTime == AnimationTime{100});
        CHECK(forward.Value().direction == AnimationTraversalDirection::Forward);
        CHECK(forward.Value().boundaryCrossings == 1);
        const auto held = clip.Traverse(forward.Value().cursor, {-50});
        REQUIRE(held.HasValue());
        CHECK(held.Value().cursor == forward.Value().cursor);
        CHECK(held.Value().direction == AnimationTraversalDirection::Held);

        const auto reverse = clip.Traverse({{10}, false}, {-20});
        REQUIRE(reverse.HasValue());
        CHECK(reverse.Value().cursor == AnimationClipCursor{{0}, true});
        CHECK(reverse.Value().direction == AnimationTraversalDirection::Reverse);
    }

    TEST_CASE("Loop traversal supports forward reverse and multi-loop exact intervals", "[unit][animation][clip][wrap]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton, AnimationWrapMode::Loop);
        const auto forward = clip.Traverse({{90}, false}, {20});
        REQUIRE(forward.HasValue());
        CHECK(forward.Value().cursor.phase == AnimationTime{10});
        CHECK(forward.Value().boundaryCrossings == 1);
        CHECK(forward.Value().direction == AnimationTraversalDirection::Forward);
        const auto reverse = clip.Traverse({{10}, false}, {-20});
        REQUIRE(reverse.HasValue());
        CHECK(reverse.Value().cursor.phase == AnimationTime{90});
        CHECK(reverse.Value().boundaryCrossings == 1);
        CHECK(reverse.Value().direction == AnimationTraversalDirection::Reverse);
        const auto large = clip.Traverse({{5}, false}, {305});
        REQUIRE(large.HasValue());
        CHECK(large.Value().cursor.phase == AnimationTime{10});
        CHECK(large.Value().boundaryCrossings == 3);
    }

    TEST_CASE("Ping-pong traversal reflects at both boundaries in either playback direction", "[unit][animation][clip][wrap]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton, AnimationWrapMode::PingPong);
        const auto forward = clip.Traverse({{90}, false}, {20});
        REQUIRE(forward.HasValue());
        CHECK(forward.Value().cursor.phase == AnimationTime{110});
        CHECK(forward.Value().sampleTime == AnimationTime{90});
        CHECK(forward.Value().direction == AnimationTraversalDirection::Reverse);
        CHECK(forward.Value().boundaryCrossings == 1);
        const auto reverse = clip.Traverse({{10}, false}, {-20});
        REQUIRE(reverse.HasValue());
        CHECK(reverse.Value().cursor.phase == AnimationTime{190});
        CHECK(reverse.Value().sampleTime == AnimationTime{10});
        CHECK(reverse.Value().direction == AnimationTraversalDirection::Forward);
        CHECK(reverse.Value().boundaryCrossings == 1);
    }

    TEST_CASE("ClampForever holds a boundary without terminating and can traverse away later", "[unit][animation][clip][wrap]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton, AnimationWrapMode::ClampForever);
        const auto clamped = clip.Traverse({{90}, false}, {20});
        REQUIRE(clamped.HasValue());
        CHECK(clamped.Value().cursor == AnimationClipCursor{{100}, false});
        const auto returned = clip.Traverse(clamped.Value().cursor, {-25});
        REQUIRE(returned.HasValue());
        CHECK(returned.Value().cursor.phase == AnimationTime{75});
        CHECK(returned.Value().direction == AnimationTraversalDirection::Reverse);
    }

    TEST_CASE("Large traversal preflight and exact addition overflow fail without a candidate cursor", "[unit][animation][clip][limits]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton);
        AnimationClipLimits oneCrossing{};
        oneCrossing.maximumBoundaryCrossings = 1;
        RequireClipError(clip.Traverse({{5}, false}, {205}, oneCrossing), AnimationErrors::ClipLimitExceeded);
        RequireClipError(clip.Traverse({{99}, false}, {std::numeric_limits<std::int64_t>::max()}), AnimationErrors::ClipTimeOverflow);
        AnimationClipLimits invalid{};
        invalid.maximumBoundaryCrossings = AnimationClipHardLimits::BoundaryCrossings + 1U;
        RequireClipError(clip.Traverse({}, {}, invalid), AnimationErrors::ClipLimitExceeded);
    }

    TEST_CASE("Clip creation rejects malformed metadata tracks and finite-limit violations", "[unit][animation][clip][validation]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto version = ClipData();
        version.descriptor.contractVersion.minor = 1;
        RequireClipError(AnimationClipAsset::Create(std::move(version), skeleton), AnimationErrors::ClipVersionUnsupported);
        auto rate = ClipData();
        rate.descriptor.sampleRate = {30, 2};
        RequireClipError(AnimationClipAsset::Create(std::move(rate), skeleton), AnimationErrors::ClipMalformed);
        auto unknown = ClipData();
        unknown.descriptor.wrapMode = static_cast<AnimationWrapMode>(255);
        RequireClipError(AnimationClipAsset::Create(std::move(unknown), skeleton), AnimationErrors::ClipUnsupported);
        auto duplicateTrack = ClipData();
        duplicateTrack.tracks.push_back(duplicateTrack.tracks.front());
        RequireClipError(AnimationClipAsset::Create(std::move(duplicateTrack), skeleton), AnimationErrors::ClipDuplicateIdentity);
        auto duplicateKey = ClipData();
        duplicateKey.tracks.front().keys.push_back(Key(0, 5.0F));
        RequireClipError(AnimationClipAsset::Create(std::move(duplicateKey), skeleton), AnimationErrors::ClipDuplicateIdentity);
        auto missingJoint = ClipData();
        missingJoint.tracks.front().joint = Id<JointId>(999);
        RequireClipError(AnimationClipAsset::Create(std::move(missingJoint), skeleton), AnimationErrors::ClipJointMissing);
        auto nonFinite = ClipData();
        nonFinite.tracks.front().keys.front().transform.translation.x = std::numeric_limits<float>::quiet_NaN();
        RequireClipError(AnimationClipAsset::Create(std::move(nonFinite), skeleton), AnimationErrors::ClipMalformed);
    }

    TEST_CASE("Absolute and additive descriptor unions reject contradictory reference-pose metadata",
              "[unit][animation][clip][additive][validation]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto absoluteReference = ClipData();
        absoluteReference.descriptor.referencePose =
            AnimationReferencePoseBinding{Id<AnimationReferencePoseId>(45), Id<AnimationReferencePoseGeneration>(46)};
        RequireClipError(AnimationClipAsset::Create(std::move(absoluteReference), skeleton), AnimationErrors::ClipMalformed);
        auto additiveMissing = ClipData(AnimationWrapMode::Loop, AnimationClipKind::Additive);
        additiveMissing.descriptor.referencePose.reset();
        RequireClipError(AnimationClipAsset::Create(std::move(additiveMissing), skeleton), AnimationErrors::ClipReferencePoseMismatch);
    }

    TEST_CASE("Reload cancellation and shutdown fail closed before malformed candidate publication", "[unit][animation][clip][lifecycle]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        auto malformed = ClipData();
        malformed.tracks.clear();
        RequireClipError(AnimationClipAsset::Create(malformed, skeleton, {.admission = AnimationClipAdmissionState::CancellationRequested}),
                         AnimationErrors::ClipOperationCancelled);
        RequireClipError(AnimationClipAsset::Create(malformed, skeleton, {.admission = AnimationClipAdmissionState::ShuttingDown}),
                         AnimationErrors::ClipAdmissionRejected);
        AnimationClipBuildContext reload{.replacing = Asset<AnimationClipId>(99)};
        RequireClipError(AnimationClipAsset::Create(ClipData(), skeleton, reload), AnimationErrors::ClipReloadMismatch);
    }

    TEST_CASE("Sampling generation and additive reference fences reject stale work without output mutation",
              "[unit][animation][clip][stale]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton);
        std::vector<Math::Transform> output(3);
        output[0].translation.x = 77.0F;
        auto context = SampleContext(output);
        context.clipGeneration = Id<AnimationClipGeneration>(999);
        RequireClipError(clip.Sample({50}, context), AnimationErrors::ClipBindingStale);
        CHECK(output[0].translation.x == 77.0F);

        const AnimationClipAsset additive = Clip(skeleton, AnimationWrapMode::Loop, AnimationClipKind::Additive);
        std::vector<Math::Transform> base(3);
        std::vector<Math::Transform> reference(3);
        context = AdditiveSampleContext(output, base, reference, Id<AnimationReferencePoseGeneration>(999));
        RequireClipError(additive.Sample({50}, context), AnimationErrors::ClipReferencePoseMismatch);
        CHECK(output[0].translation.x == 77.0F);
    }

    TEST_CASE("Additive sampling permits exact base output alias and rejects reference output alias transactionally",
              "[unit][animation][clip][additive][alias]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton, AnimationWrapMode::Loop, AnimationClipKind::Additive);
        std::array<Math::Transform, 3> base{};
        std::array<Math::Transform, 3> reference{};
        base[1].translation.x = 10.0F;
        const auto inPlace = AdditiveSampleContext(base, base, reference);
        REQUIRE(clip.Sample({50}, inPlace).HasValue());
        CHECK(base[1].translation.x == 15.0F);

        reference[0].translation.x = 77.0F;
        const auto invalid = AdditiveSampleContext(reference, base, reference);
        RequireClipError(clip.Sample({50}, invalid), AnimationErrors::ClipMalformed);
        CHECK(reference[0].translation.x == 77.0F);
    }

    TEST_CASE("Sampling and traversal cancellation shutdown malformed cursor and sizes are deterministic",
              "[unit][animation][clip][failure]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton);
        std::vector<Math::Transform> output(3);
        auto context = SampleContext(output);
        context.admission = AnimationClipAdmissionState::CancellationRequested;
        RequireClipError(clip.Sample({50}, context), AnimationErrors::ClipOperationCancelled);
        context.admission = AnimationClipAdmissionState::ShuttingDown;
        RequireClipError(clip.Sample({50}, context), AnimationErrors::ClipAdmissionRejected);
        RequireClipError(clip.Traverse({{100}, false}, {1}), AnimationErrors::ClipMalformed);
        RequireClipError(clip.Traverse({}, {}, {}, AnimationClipAdmissionState::CancellationRequested),
                         AnimationErrors::ClipOperationCancelled);
        context = SampleContext(output);
        context.outputPose = std::span<Math::Transform>{output}.first(2);
        RequireClipError(clip.Sample({50}, context), AnimationErrors::ClipLimitExceeded);
        RequireClipError(clip.Sample({101}, SampleContext(output)), AnimationErrors::ClipMalformed);
    }

    TEST_CASE("Validated clip traversal and sampling perform no frame-hot allocations", "[unit][animation][clip][allocation]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton, AnimationWrapMode::PingPong);
        std::vector<Math::Transform> output(3);
        auto context = SampleContext(output);
        REQUIRE(clip.Sample({50}, context).HasValue());
        const std::size_t before = ClipTestAllocations.load(std::memory_order_relaxed);
        const auto traversal = clip.Traverse({{95}, false}, {20});
        REQUIRE(traversal.HasValue());
        const auto sample = clip.Sample(traversal.Value().sampleTime, context);
        REQUIRE(sample.HasValue());
        CHECK(ClipTestAllocations.load(std::memory_order_relaxed) == before);
    }

    TEST_CASE("Immutable clip sampling is safe for independent concurrent output storage", "[unit][animation][clip][concurrency]") {
        const SkeletonAsset skeleton = ClipSkeleton();
        const AnimationClipAsset clip = Clip(skeleton);
        std::array<std::array<Math::Transform, 3>, 4> outputs{};
        std::array<bool, 4> succeeded{};
        std::array<std::thread, 4> readers;
        for (std::size_t index = 0; index < readers.size(); ++index) {
            readers[index] = std::thread([&, index] {
                succeeded[index] = clip.Sample({50}, SampleContext(outputs[index])).HasValue();
            });
        }
        for (std::thread &reader : readers)
            reader.join();
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            CHECK(succeeded[index]);
            CHECK(outputs[index][1].translation.x == 5.0F);
        }
    }

    TEST_CASE("Animation held ping-pong traversal samples the actual reverse-half phase", "[animation][clip][traversal]") {
        const auto skeleton = ClipSkeleton();
        const auto clip = Clip(skeleton, AnimationWrapMode::PingPong);
        const auto held = clip.TraversePrepared({{150}, false}, {0});
        REQUIRE(held.HasValue());
        CHECK(held.Value().cursor.phase.ticks == 150);
        CHECK_FALSE(held.Value().cursor.terminal);
        CHECK(held.Value().sampleTime.ticks == 50);
        CHECK(held.Value().direction == AnimationTraversalDirection::Held);
        CHECK(held.Value().boundaryCrossings == 0);
    }

}  // namespace Horo::Animation
