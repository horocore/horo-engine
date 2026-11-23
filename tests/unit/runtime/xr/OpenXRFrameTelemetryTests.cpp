#include "OpenXRFrameCompositorTestFixture.h"
#include "OpenXRFrameMetricsTestSupport.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Tests::OpenXR {
#if HORO_ENABLE_TELEMETRY
    TEST_CASE("Native XR metrics preserve failed and empty outcomes without physical completion claims", "[xr][native][telemetry]") {
        FrameMetricRecorder metrics;
        metrics.Start();
        bool waitFailed{};
        bool endFailed{};
        SECTION("successful empty frame") {}
        SECTION("failed wait") {
            waitFailed = true;
        }
        SECTION("failed end followed by explicit recovery") {
            endFailed = true;
        }
        {
            CompositorFixture fixture;
            REQUIRE(fixture.compositor.Bind({&FrameScript::GetProc}, {}, NativeCompositorColorSpace::RuntimeDefault, metrics.handles)
                        .HasValue());
            if (waitFailed) {
                fixture.script.waitResult = XR_ERROR_RUNTIME_FAILURE;
                CHECK(fixture.compositor.Wait().HasError());
            } else {
                const auto frame = fixture.Begin();
                if (endFailed) {
                    REQUIRE(fixture.frames.Abort(frame) == XRFrameStatus::Ok);
                    fixture.script.endResult = XR_ERROR_RUNTIME_FAILURE;
                    CHECK(fixture.compositor.EndEmpty(frame).HasError());
                    CHECK(fixture.compositor.RetainedFault());
                    fixture.Recover();
                } else {
                    fixture.Empty(frame);
                }
            }
        }
        metrics.Finish();
        CHECK(metrics.sink->Total(NativeFrameMetricOutcome::WaitFailed) == (waitFailed ? 1 : 0));
        CHECK(metrics.sink->Total(NativeFrameMetricOutcome::WaitCompleted) == (waitFailed ? 0 : 1));
        CHECK(metrics.sink->Total(NativeFrameMetricOutcome::BeginCompleted) == (waitFailed ? 0 : 1));
        CHECK(metrics.sink->Total(NativeFrameMetricOutcome::EndFailed) == (endFailed ? 1 : 0));
        CHECK(metrics.sink->Total(NativeFrameMetricOutcome::EmptyAccepted) == (!waitFailed && !endFailed ? 1 : 0));
        CHECK(metrics.sink->Total(NativeFrameMetricOutcome::ProjectionAccepted) == 0);
        bool periodObserved{};
        for (std::size_t index = 0; index < metrics.sink->records.size(); ++index) {
            const auto &descriptor = metrics.sink->descriptors[index];
            CHECK(descriptor.subsystem == "XR.OpenXR");
            if (descriptor.name == "horo.xr.native_frame") {
                REQUIRE(descriptor.dimensions.size() == 1);
                CHECK(descriptor.dimensions[0].key == "outcome");
                CHECK(descriptor.dimensions[0].allowedValues.size() == 8);
                CHECK(descriptor.maxSeries == 8);
            } else {
                CHECK(descriptor.dimensions.empty());
            }
            if (descriptor.name == "horo.xr.predicted_period") {
                periodObserved = true;
                CHECK(metrics.sink->records[index].value == Catch::Approx(0.011111111));
            }
        }
        CHECK(periodObserved == !waitFailed);
    }

    TEST_CASE("Native XR swapchain metrics count timeouts and retries without image identities", "[xr][native][telemetry]") {
        using enum NativeSwapchainMetricOutcome;
        FrameMetricRecorder metrics;
        metrics.Start();
        {
            CompositorFixture fixture(false, false, false, NativeReprojectionRoute::SpaceWarpFb, false, 1,
                                      XRReprojectionCadenceMode::FixedInterval, metrics.handles.swapchain);
            CHECK(fixture.color.Wait(0).HasError());
            fixture.swapScript.fail = "acquire";
            CHECK(fixture.color.Acquire().HasError());
            fixture.swapScript.fail = {};
            REQUIRE(fixture.color.Acquire().HasValue());
            fixture.swapScript.timeout = true;
            const auto timed = fixture.color.Wait(0);
            REQUIRE(timed.HasValue());
            CHECK_FALSE(timed.Value());
            fixture.swapScript.timeout = false;
            fixture.swapScript.fail = "wait";
            CHECK(fixture.color.Wait(0).HasError());
            fixture.swapScript.fail = {};
            REQUIRE(fixture.color.Wait(0).Value());
            fixture.swapScript.fail = "release";
            CHECK(fixture.color.PollRelease().HasError());
            fixture.swapScript.fail = {};
            REQUIRE(fixture.color.PollRelease().Value());
        }
        metrics.Finish();
        for (const auto outcome : {AcquireCompleted, AcquireFailed, WaitTimeout, WaitReady, WaitFailed, ReleaseCompleted, ReleaseFailed})
            CHECK(metrics.sink->Total(outcome) == 1);
        for (const auto &descriptor : metrics.sink->descriptors) {
            REQUIRE(descriptor.name == "horo.xr.native_swapchain");
            REQUIRE(descriptor.dimensions.size() == 1);
            CHECK(descriptor.dimensions.front().key == "outcome");
            CHECK(descriptor.maxSeries == 7);
        }
    }

    TEST_CASE("XR color coverage is sampled only after native projection acceptance", "[xr][native][telemetry][resolution]") {
        bool rejected{};
        bool saturated{};
        SECTION("runtime accepts the scaled projection") {
            // The fixture defaults to a successful native submission and an available metrics queue.
        }
        SECTION("runtime rejects the same projection") {
            rejected = true;
        }
        SECTION("runtime accepts while telemetry drops records") {
            saturated = true;
        }
        FrameMetricRecorder metrics;
        metrics.Start(saturated ? 2U : 128U);
        {
            CompositorFixture fixture;
            REQUIRE(fixture.compositor.Bind({&FrameScript::GetProc}, {}, NativeCompositorColorSpace::RuntimeDefault, metrics.handles)
                        .HasValue());
            const auto frame = fixture.Begin();
            const auto image = fixture.Acquire(frame);
            const auto plan = fixture.Plan(frame, image, nullptr, false, 85);
            fixture.images.complete = true;
            const std::array submitted{image};
            REQUIRE(fixture.frames.Submit(frame, submitted) == XRFrameStatus::Ok);
            REQUIRE(fixture.color.PollRelease().Value());
            REQUIRE(fixture.frames.Release(frame, image) == XRFrameStatus::Ok);
            if (rejected)
                fixture.script.endResult = XR_ERROR_RUNTIME_FAILURE;
            const std::array targets{NativeProjectionTarget{&fixture.color, image}};
            const auto dropped = Telemetry::Runtime::GetStatistics().droppedRecords;
            CHECK(fixture.compositor.EndProjection(frame, plan, targets).HasError() == rejected);
            if (saturated)
                CHECK(Telemetry::Runtime::GetStatistics().droppedRecords > dropped);
            if (rejected)
                fixture.Recover();
        }
        metrics.Finish();
        std::size_t count{};
        for (std::size_t index = 0; index < metrics.sink->descriptors.size(); ++index) {
            const auto &descriptor = metrics.sink->descriptors[index];
            if (descriptor.name != "horo.xr.projection_color_coverage")
                continue;
            ++count;
            CHECK(descriptor.unit == Telemetry::MetricUnit::Ratio);
            CHECK(descriptor.dimensions.empty());
            CHECK(descriptor.maxSeries == 1);
            CHECK(metrics.sink->records[index].value == Catch::Approx(0.85 * 0.85));
        }
        CHECK(count == (rejected || saturated ? 0U : 1U));
    }

    TEST_CASE("XR fixed foveation metrics describe the published plan only", "[xr][native][telemetry]") {
        using enum XRFeatureDecisionState;
        FrameMetricRecorder metrics;
        metrics.Start();
        metrics.handles.RecordFixedFoveationPlan(NotRequested);
        metrics.handles.RecordFixedFoveationPlan(OptionalEnabled);
        metrics.handles.RecordFixedFoveationPlan(OptionalDisabled);
        metrics.handles.RecordFixedFoveationPlan(static_cast<XRFeatureDecisionState>(255));
        metrics.Finish();

        CHECK(metrics.sink->Total(NotRequested) == 1);
        CHECK(metrics.sink->Total(OptionalEnabled) == 1);
        CHECK(metrics.sink->Total(OptionalDisabled) == 1);
        CHECK(metrics.sink->Total(Required) == 0);
        CHECK(metrics.sink->Total(BaselineProjection) == 0);
        REQUIRE(metrics.sink->records.size() == 3);
        for (const auto &descriptor : metrics.sink->descriptors) {
            REQUIRE(descriptor.name == "horo.xr.fixed_foveation_plan");
            REQUIRE(descriptor.dimensions.size() == 1);
            CHECK(descriptor.maxSeries == 5);
        }
    }

    TEST_CASE("XR Stage metrics expose classifications and omit malformed samples", "[xr][native][telemetry]") {
        using enum XRStageBoundaryProximity;
        FrameMetricRecorder metrics;
        metrics.Start();
        metrics.handles.RecordStageBoundary(Inside);
        metrics.handles.RecordStageBoundary(Near);
        metrics.handles.RecordStageBoundary(Outside);
        metrics.handles.RecordStageBoundary(static_cast<XRStageBoundaryProximity>(255));
        metrics.Finish();
        CHECK(metrics.sink->Total(Inside) == 1);
        CHECK(metrics.sink->Total(Near) == 1);
        CHECK(metrics.sink->Total(Outside) == 1);
        REQUIRE(metrics.sink->records.size() == 3);
        for (const auto &descriptor : metrics.sink->descriptors) {
            REQUIRE(descriptor.name == "horo.xr.stage_boundary");
            REQUIRE(descriptor.dimensions.size() == 1);
            CHECK(descriptor.maxSeries == 3);
        }
    }

    TEST_CASE("XR comfort telemetry exposes only accepted projection outcomes", "[xr][native][telemetry][comfort]") {
        FrameMetricRecorder metrics;
        metrics.Start();
        metrics.handles.RecordAcceptedProjectionComfort(true);
        metrics.handles.RecordAcceptedProjectionComfort(false);
        metrics.handles.RecordAcceptedProjectionComfort(true);
        metrics.Finish();
        CHECK(metrics.sink->Total("horo.xr.projection_comfort", 0) == 1);
        CHECK(metrics.sink->Total("horo.xr.projection_comfort", 1) == 2);
        REQUIRE(metrics.sink->records.size() == 3);
        for (const auto &descriptor : metrics.sink->descriptors) {
            CHECK(descriptor.name == "horo.xr.projection_comfort");
            REQUIRE(descriptor.dimensions.size() == 1);
            CHECK(descriptor.dimensions[0].key == "outcome");
            CHECK((descriptor.dimensions[0].allowedValues == std::vector<std::string>{"without_vignette", "with_vignette"}));
            CHECK(descriptor.maxSeries == 2);
        }
    }

    TEST_CASE("XR timing metrics correlate cutoffs with the exact native prediction", "[xr][native][telemetry]") {
        CHECK_FALSE(NativeFrameMetricHandles{}.MeasuresRenderCompletion());
        FrameMetricRecorder metrics;
        metrics.Start();
        REQUIRE(metrics.handles.MeasuresRenderCompletion());
        {
            CompositorFixture fixture;
            fixture.Bind();
            const auto frame = fixture.Begin();
            const auto prediction = fixture.frames.Snapshot().predictedDisplayTime;
            const auto early =
                fixture.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(prediction.Nanoseconds() - 2'000'000));
            const auto lateCpu =
                fixture.compositor.ObservePacing(frame, IdentityValue<XRRenderPredictionTime>(prediction.Nanoseconds() - 1));
            const auto lateRender = fixture.compositor.ObservePacing(frame, prediction);
            REQUIRE(early.HasValue());
            REQUIRE(lateCpu.HasValue());
            REQUIRE(lateRender.HasValue());
            for (const auto deadline : {early.Value().deadline, lateCpu.Value().deadline, lateRender.Value().deadline}) {
                metrics.handles.RecordCpuDeadline(deadline);
                metrics.handles.RecordRenderCompletion(deadline);
            }
            fixture.Empty(frame);
            CHECK(fixture.compositor.ObservePacing(frame, prediction).HasError());
        }
        metrics.handles.RecordCpuDeadline(std::nullopt);
        metrics.handles.RecordRenderCompletion(std::nullopt);
        metrics.Finish();
        for (unsigned index = 0; index < 4; ++index)
            CHECK(metrics.sink->Total("horo.xr.cpu_deadline", index) == 1);
        CHECK(metrics.sink->Total("horo.xr.render_completion", 0) == 2);
        CHECK(metrics.sink->Total("horo.xr.render_completion", 1) == 1);
        CHECK(metrics.sink->Total("horo.xr.render_completion", 2) == 1);
        REQUIRE(metrics.sink->records.size() == 8);
        for (const auto &descriptor : metrics.sink->descriptors) {
            REQUIRE(descriptor.dimensions.size() == 1);
            CHECK(descriptor.maxSeries == (descriptor.name == "horo.xr.cpu_deadline" ? 4U : 3U));
        }
    }
#endif
}  // namespace Horo::Tests::OpenXR
