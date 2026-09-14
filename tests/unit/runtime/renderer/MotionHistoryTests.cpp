#include "Horo/Runtime/Render/MotionHistory.h"
#include "Horo/Runtime/Render/MotionHistoryErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <future>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] TemporalHistoryCompatibility Compatibility() {
        return {
            .view = {1},
            .provider = {2},
            .mode = {3},
            .renderExtent = {1280, 720},
            .targetExtent = {1920, 1080},
            .providerGeneration = 1,
            .modeGeneration = 1,
            .surfaceGeneration = 1,
            .rasterGeneration = 1,
            .colorGeneration = 1,
            .exposureGeneration = 1,
            .inputSchemaGeneration = 1,
            .deviceGeneration = 1,
            .projectionGeneration = 1,
            .jitterGeneration = 1,
            .sceneOriginGeneration = 1,
            .motionGeneration = 1,
            .recipeGeneration = 1,
        };
    }

    [[nodiscard]] RenderTextureHandle Texture(const std::uint32_t slot) {
        return {{7}, slot, 2};
    }

    [[nodiscard]] RenderMotionObjectSample Object(const std::uint64_t id, const float x) {
        return {{id}, Math::TranslationMatrix({x, 0.0F, 0.0F})};
    }

    [[nodiscard]] RenderMotionFrameRequest Request(const std::uint64_t frameId, const std::uint64_t predecessor,
                                                   const std::span<const RenderMotionObjectSample> objects) {
        return {
            .compatibility = Compatibility(),
            .frameId = frameId,
            .predecessorFrameId = predecessor,
            .camera = {Math::TranslationMatrix({static_cast<float>(frameId), 0.0F, 0.0F}), {0.01F, -0.02F}},
            .disocclusion = {Texture(1), Texture(2), Texture(3), std::nullopt},
            .objects = objects,
        };
    }

    [[nodiscard]] RenderMotionHistoryTracker Tracker(const std::size_t maximumObjects = 8) {
        auto created = RenderMotionHistoryTracker::Create({maximumObjects});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        Tests::RequireActionableError(result, expected);
    }

    struct ResetMutation {
        TemporalHistoryResetCause expected;
        void (*apply)(RenderMotionFrameRequest &);
    };

    void RequireReset(const ResetMutation &mutation) {
        const std::array objects{Object(1, 1.0F)};
        auto tracker = Tracker();
        auto first = tracker.BeginFrame(Request(1, 0, objects));
        REQUIRE(first.HasValue());
        REQUIRE(tracker.Publish(first.Value()).HasValue());
        auto changedRequest = Request(2, 1, objects);
        mutation.apply(changedRequest);
        auto changed = tracker.BeginFrame(changedRequest);
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().resetCause == mutation.expected);
        CHECK_FALSE(changed.Value().camera.hasPrevious);
        CHECK_FALSE(changed.Value().objects.front().hasPrevious);
    }
}  // namespace

TEST_CASE("Motion history validates bounds and semantic resource generations", "[runtime][renderer][motion-history]") {
    RequireError(RenderMotionHistoryTracker::Create({0}), MotionHistoryErrors::InvalidLimits);
    RequireError(RenderMotionHistoryTracker::Create({RenderMotionHistoryLimits::HardMaxObjects + 1}), MotionHistoryErrors::InvalidLimits);

    auto tracker = Tracker(1);
    const std::array tooMany{Object(1, 0.0F), Object(2, 0.0F)};
    RequireError(tracker.BeginFrame(Request(1, 0, tooMany)), MotionHistoryErrors::InvalidRequest);

    const std::array duplicate{Object(2, 0.0F), Object(2, 1.0F)};
    RequireError(tracker.BeginFrame(Request(1, 0, duplicate)), MotionHistoryErrors::InvalidRequest);

    const std::array one{Object(1, 0.0F)};
    auto invalidResource = Request(1, 0, one);
    invalidResource.disocclusion.motionVectors = {};
    RequireError(tracker.BeginFrame(invalidResource), MotionHistoryErrors::InvalidRequest);

    auto pending = tracker.BeginFrame(Request(1, 0, one));
    REQUIRE(pending.HasValue());
    RequireError(tracker.BeginFrame(Request(2, 1, one)), MotionHistoryErrors::FrameAlreadyPending);
}

TEST_CASE("Successful real frames publish canonical object and camera motion", "[runtime][renderer][motion-history]") {
    auto tracker = Tracker();
    const std::array firstObjects{Object(20, 2.0F), Object(10, 1.0F)};
    auto first = tracker.BeginFrame(Request(10, 0, firstObjects));
    REQUIRE(first.HasValue());
    CHECK(first.Value().convention == RenderMotionVectorConvention::PreviousUvMinusCurrentUv);
    CHECK(first.Value().resetCause == TemporalHistoryResetCause::FirstFrame);
    CHECK_FALSE(first.Value().camera.hasPrevious);
    REQUIRE(first.Value().objects.size() == 2);
    CHECK(first.Value().objects[0].object == RenderMotionObjectId{10});
    CHECK_FALSE(first.Value().objects[0].hasPrevious);
    REQUIRE(tracker.Publish(first.Value()).HasValue());

    const std::array secondObjects{Object(30, 8.0F), Object(10, 4.0F)};
    auto second = tracker.BeginFrame(Request(11, 10, secondObjects));
    REQUIRE(second.HasValue());
    CHECK_FALSE(second.Value().resetCause.has_value());
    CHECK(second.Value().camera.hasPrevious);
    CHECK(second.Value().camera.previousUnjitteredViewProjection == first.Value().camera.currentUnjitteredViewProjection);
    REQUIRE(second.Value().objects.size() == 2);
    CHECK(second.Value().objects[0].object == RenderMotionObjectId{10});
    CHECK(second.Value().objects[0].hasPrevious);
    CHECK(second.Value().objects[0].previousLocalToWorld == Object(10, 1.0F).localToWorld);
    CHECK(second.Value().objects[1].object == RenderMotionObjectId{30});
    CHECK_FALSE(second.Value().objects[1].hasPrevious);
}

TEST_CASE("Abandoned and tampered attempts never advance published history", "[runtime][renderer][motion-history]") {
    auto tracker = Tracker();
    const std::array initial{Object(1, 1.0F)};
    auto first = tracker.BeginFrame(Request(1, 0, initial));
    REQUIRE(first.HasValue());
    REQUIRE(tracker.Publish(first.Value()).HasValue());

    const std::array abandonedObjects{Object(1, 9.0F)};
    auto abandoned = tracker.BeginFrame(Request(2, 1, abandonedObjects));
    REQUIRE(abandoned.HasValue());
    auto tampered = abandoned.Value();
    tampered.objects.front().currentLocalToWorld = Math::Mat4::Identity();
    RequireError(tracker.Publish(tampered), MotionHistoryErrors::InvalidFrame);
    REQUIRE(tracker.Abandon(abandoned.Value()).HasValue());

    const std::array resumedObjects{Object(1, 3.0F)};
    auto resumed = tracker.BeginFrame(Request(3, 1, resumedObjects));
    REQUIRE(resumed.HasValue());
    CHECK(resumed.Value().objects.front().previousLocalToWorld == initial.front().localToWorld);
}

TEST_CASE("Explicit discontinuities have stable reset causes", "[runtime][renderer][motion-history]") {
    const std::array mutations{
        ResetMutation{TemporalHistoryResetCause::InvalidInput,
                      [](auto &r) {
        r.invalidation.invalidInput = true;
    }},
        ResetMutation{TemporalHistoryResetCause::CameraCut,
                      [](auto &r) {
        r.invalidation.cameraCut = true;
    }},
        ResetMutation{TemporalHistoryResetCause::Suspension,
                      [](auto &r) {
        r.invalidation.suspended = true;
    }},
        ResetMutation{TemporalHistoryResetCause::ExplicitRequest,
                      [](auto &r) {
        r.invalidation.explicitRequest = true;
    }},
        ResetMutation{TemporalHistoryResetCause::SkippedFrame,
                      [](auto &r) {
        r.invalidation.skippedFrame = true;
    }},
        ResetMutation{TemporalHistoryResetCause::ProviderRequested,
                      [](auto &r) {
        r.invalidation.providerRequested = true;
    }},
    };
    for (const ResetMutation &mutation : mutations)
        RequireReset(mutation);
}

TEST_CASE("Identity and image compatibility changes have stable reset causes", "[runtime][renderer][motion-history]") {
    const std::array mutations{
        ResetMutation{TemporalHistoryResetCause::ViewReplacement,
                      [](auto &r) {
        r.compatibility.view = {9};
    }},
        ResetMutation{TemporalHistoryResetCause::SurfaceReplacement,
                      [](auto &r) {
        ++r.compatibility.surfaceGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::DeviceReplacement,
                      [](auto &r) {
        ++r.compatibility.deviceGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::ProviderReplacement,
                      [](auto &r) {
        ++r.compatibility.providerGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::ModeReplacement,
                      [](auto &r) {
        ++r.compatibility.modeGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::Resize,
                      [](auto &r) {
        ++r.compatibility.renderExtent.width;
    }},
        ResetMutation{TemporalHistoryResetCause::SchemaReplacement,
                      [](auto &r) {
        ++r.compatibility.jitterGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::ProjectionChange,
                      [](auto &r) {
        ++r.compatibility.projectionGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::ColorPlanChange,
                      [](auto &r) {
        ++r.compatibility.colorGeneration;
    }},
    };
    for (const ResetMutation &mutation : mutations)
        RequireReset(mutation);
}

TEST_CASE("Scene and recipe compatibility changes have stable reset causes", "[runtime][renderer][motion-history]") {
    const std::array mutations{
        ResetMutation{TemporalHistoryResetCause::SceneDiscontinuity,
                      [](auto &r) {
        ++r.compatibility.sceneOriginGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::ProfileChange,
                      [](auto &r) {
        ++r.compatibility.rasterGeneration;
    }},
        ResetMutation{TemporalHistoryResetCause::RecipeChange,
                      [](auto &r) {
        ++r.compatibility.recipeGeneration;
    }},
    };
    for (const ResetMutation &mutation : mutations)
        RequireReset(mutation);
}

TEST_CASE("Missing predecessors reset motion while shutdown and thread affinity remain explicit", "[runtime][renderer][motion-history]") {
    auto tracker = Tracker();
    const std::array objects{Object(1, 1.0F)};
    auto first = tracker.BeginFrame(Request(5, 0, objects));
    REQUIRE(first.HasValue());
    REQUIRE(tracker.Publish(first.Value()).HasValue());

    auto skipped = tracker.BeginFrame(Request(7, 4, objects));
    REQUIRE(skipped.HasValue());
    CHECK(skipped.Value().resetCause == TemporalHistoryResetCause::MissingPredecessor);
    REQUIRE(tracker.Abandon(skipped.Value()).HasValue());

    auto wrongThread = std::async(std::launch::async, [&tracker] {
        return tracker.Shutdown();
    }).get();
    RequireError(wrongThread, MotionHistoryErrors::WrongThread);
    REQUIRE(tracker.Shutdown().HasValue());
    REQUIRE(tracker.Shutdown().HasValue());
    RequireError(tracker.BeginFrame(Request(8, 5, objects)), MotionHistoryErrors::TrackerStopped);
}
