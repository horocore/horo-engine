#include "host/OpenXRFrameTelemetry.h"

#include "Horo/XR/XRFeatureNegotiation.h"
#include "Horo/XR/XRInteraction.h"
#include "host/OpenXRFrameCompositor.h"

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Registers the complete finite outcome table off the native frame path. */
        template <std::size_t Count>
        std::array<Telemetry::Counter, Count> RegisterOutcomes(const char *name, const char *description,
                                                               const std::array<const char *, Count> &names) {
            const auto counter = Telemetry::Runtime::RegisterCounter({.name = name,
                                                                      .subsystem = "XR.OpenXR",
                                                                      .unit = Telemetry::MetricUnit::Count,
                                                                      .description = description,
                                                                      .dimensions = {{"outcome", {names.begin(), names.end()}}},
                                                                      .maxSeries = static_cast<std::uint32_t>(Count)});
            std::array<Telemetry::Counter, Count> bound;
            for (std::size_t index = 0; index < Count; ++index) {
                const std::array dimension{Telemetry::DimensionValue{"outcome", names[index]}};
                bound[index] = counter.WithDimensions(dimension);
            }
            return bound;
        }
    }  // namespace

    /** @copydoc RegisterNativeFrameMetricHandles */
    NativeFrameMetricHandles RegisterNativeFrameMetricHandles() {
        constexpr std::array names{"wait_completed", "wait_failed",         "begin_completed", "begin_discarded",
                                   "begin_failed",   "projection_accepted", "empty_accepted",  "end_failed"};
        static_assert(names.size() == static_cast<std::size_t>(NativeFrameMetricOutcome::Count));
        constexpr std::array swapchainNames{"acquire_completed", "acquire_failed",    "wait_ready",    "wait_timeout",
                                            "wait_failed",       "release_completed", "release_failed"};
        static_assert(swapchainNames.size() == static_cast<std::size_t>(NativeSwapchainMetricOutcome::Count));
        constexpr std::array planDecisions{"not_requested", "required", "optional_enabled", "optional_disabled", "baseline_projection"};
        static_assert(planDecisions.size() == static_cast<std::size_t>(XRFeatureDecisionState::BaselineProjection) + 1);
        NativeFrameMetricHandles handles;
        handles.outcomes = RegisterOutcomes("horo.xr.native_frame", "Actual native frame API outcomes", names);
        handles.swapchain.outcomes = RegisterOutcomes("horo.xr.native_swapchain", "Actual native swapchain API outcomes", swapchainNames);
        handles.fixedFoveationPlan =
            RegisterOutcomes("horo.xr.fixed_foveation_plan", "Published XR fixed foveation plan decisions", planDecisions);
        constexpr std::array boundaryNames{"inside", "near", "outside"};
        static_assert(boundaryNames.size() == static_cast<std::size_t>(XRStageBoundaryProximity::Outside) + 1);
        handles.stageBoundary = RegisterOutcomes("horo.xr.stage_boundary", "Committed XR Stage proximity observations", boundaryNames);
        constexpr std::array cpuDeadlines{"within_budget", "submission_deadline_passed", "display_prediction_reached",
                                          "timing_unavailable"};
        static_assert(cpuDeadlines.size() == static_cast<std::size_t>(NativeFrameDeadline::DisplayMissed) + 2);
        handles.cpuDeadline = RegisterOutcomes("horo.xr.cpu_deadline", "CPU cutoff relative to the native XR prediction", cpuDeadlines);
        constexpr std::array renderCompletions{"before_prediction", "at_or_after_prediction", "timing_unavailable"};
        handles.renderCompletion =
            RegisterOutcomes("horo.xr.render_completion", "Host observation of renderer queue completion", renderCompletions);
        constexpr std::array comfortOutcomes{"without_vignette", "with_vignette"};
        handles.projectionComfort =
            RegisterOutcomes("horo.xr.projection_comfort", "Runtime-accepted XR projection vignette outcomes", comfortOutcomes);
        handles.predictedPeriod =
            Telemetry::Runtime::RegisterHistogram({.name = "horo.xr.predicted_period",
                                                   .subsystem = "XR.OpenXR",
                                                   .unit = Telemetry::MetricUnit::Seconds,
                                                   .description = "Positive display period observed from completed native waits"});
        handles.frameDepth =
            Telemetry::Runtime::RegisterHistogram({.name = "horo.xr.native_frame_depth",
                                                   .subsystem = "XR.OpenXR",
                                                   .unit = Telemetry::MetricUnit::Count,
                                                   .description = "Per-compositor native frame depth observed at native transitions"});
        return handles;
    }

    void NativeFrameMetricHandles::RecordFixedFoveationPlan(XRFeatureDecisionState decision) const noexcept {
        const auto index = static_cast<std::size_t>(decision);
        if (index < fixedFoveationPlan.size())
            fixedFoveationPlan[index].Add();
    }

    void NativeFrameMetricHandles::RecordStageBoundary(XRStageBoundaryProximity proximity) const noexcept {
        const auto index = static_cast<std::size_t>(proximity);
        if (index < stageBoundary.size())
            stageBoundary[index].Add();
    }

    void NativeFrameMetricHandles::RecordAcceptedProjectionComfort(bool vignette) const noexcept {
        projectionComfort[vignette ? 1 : 0].Add();
    }

    void NativeFrameMetricHandles::RecordCpuDeadline(std::optional<NativeFrameDeadline> deadline) const noexcept {
        const auto unavailable = cpuDeadline.size() - 1;
        const auto index = deadline ? static_cast<std::size_t>(*deadline) : unavailable;
        cpuDeadline[index < unavailable ? index : unavailable].Add();
    }

    bool NativeFrameMetricHandles::MeasuresRenderCompletion() const noexcept {
        return renderCompletion[0] || renderCompletion[1] || renderCompletion[2];
    }

    void NativeFrameMetricHandles::RecordRenderCompletion(std::optional<NativeFrameDeadline> deadline) const noexcept {
        using enum NativeFrameDeadline;
        if (deadline == DisplayMissed)
            renderCompletion[1].Add();
        else if (deadline == WithinBudget || deadline == CpuSubmissionMissed)
            renderCompletion[0].Add();
        else
            renderCompletion[2].Add();
    }
}  // namespace Horo::XR::OpenXRInternal
