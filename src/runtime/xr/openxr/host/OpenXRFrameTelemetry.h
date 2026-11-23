#pragma once
/** @file OpenXRFrameTelemetry.h @brief Explicit host-bound measurements of native XR frame calls. */
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Horo::XR {
    class XRViewRenderPlan;
    enum class XRFeatureDecisionState : std::uint8_t;
    enum class XRStageBoundaryProximity : std::uint8_t;
}  // namespace Horo::XR

namespace Horo::XR::OpenXRInternal {
    enum class NativeFrameDeadline : std::uint8_t;
    /** @brief Closed actual native outcomes. Runtime acceptance is distinct from physical display evidence. */
    enum class NativeFrameMetricOutcome : std::uint8_t {
        WaitCompleted,
        WaitFailed,
        BeginCompleted,
        BeginDiscarded,
        BeginFailed,
        ProjectionAccepted,
        EmptyAccepted,
        EndFailed,
        Count
    };

    /** @brief Closed outcomes of actual native swapchain calls. */
    enum class NativeSwapchainMetricOutcome : std::uint8_t {
        AcquireCompleted,
        AcquireFailed,
        WaitReady,
        WaitTimeout,
        WaitFailed,
        ReleaseCompleted,
        ReleaseFailed,
        Count
    };

    struct NativeSwapchainMetricHandles final {
        std::array<Telemetry::Counter, static_cast<std::size_t>(NativeSwapchainMetricOutcome::Count)> outcomes;
    };

    /** @brief Cold-bound numeric handles copied into a session compositor. Empty or retired handles are inert. */
    struct NativeFrameMetricHandles final {
        std::array<Telemetry::Counter, static_cast<std::size_t>(NativeFrameMetricOutcome::Count)> outcomes;
        Telemetry::Histogram predictedPeriod;
        Telemetry::Histogram frameDepth;
        Telemetry::Histogram projectionCoverage;
        NativeSwapchainMetricHandles swapchain;
        std::array<Telemetry::Counter, 5> fixedFoveationPlan;
        std::array<Telemetry::Counter, 3> stageBoundary;
        std::array<Telemetry::Counter, 4> cpuDeadline;
        std::array<Telemetry::Counter, 3> renderCompletion;
        std::array<Telemetry::Counter, 2> projectionComfort;

        /** @brief Records timing at the CPU admission cutoff. */
        void RecordCpuDeadline(std::optional<NativeFrameDeadline> deadline) const noexcept;
        /** @brief Reports whether the host should sample the clock after observing renderer completion. */
        [[nodiscard]] bool MeasuresRenderCompletion() const noexcept;
        /** @brief Records when the host observed queue completion relative to the XR prediction. */
        void RecordRenderCompletion(std::optional<NativeFrameDeadline> deadline) const noexcept;

        /** @brief Records the published plan decision without claiming the renderer applied it. */
        void RecordFixedFoveationPlan(XRFeatureDecisionState decision) const noexcept;
        /** @brief Records a current Stage advisory classification without a position or distance. */
        void RecordStageBoundary(XRStageBoundaryProximity proximity) const noexcept;
        /** @brief Records a runtime-accepted projection without its opacity or input history. */
        void RecordAcceptedProjectionComfort(bool vignette) const noexcept;
        /** @brief Records native acceptance and, for projections, the fraction of allocated color pixels submitted.
         * @param projection Validated projection plan, or null for an empty frame.
         */
        void RecordAcceptedEnd(const XRViewRenderPlan *projection) const noexcept;
    };

    /** @brief Registers the finite frame vocabulary at explicit process composition before activation.
     * @return Bound native-call counters, observed prediction period and per-compositor native frame depth samples.
     * @details Dimensions contain only the closed outcome vocabulary. Tracking data and live identities are excluded.
     */
    [[nodiscard]] NativeFrameMetricHandles RegisterNativeFrameMetricHandles();
}  // namespace Horo::XR::OpenXRInternal
