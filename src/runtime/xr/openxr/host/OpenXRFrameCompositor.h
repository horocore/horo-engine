#pragma once

/** @file OpenXRFrameCompositor.h
 * @brief Non-installed native predicted-frame, projection/depth handoff and bounded pacing adapter.
 */

#include "Horo/XR/XRFrameLifecycle.h"
#include "OpenXRSwapchain.h"

#include <array>
#include <optional>
#include <thread>

namespace Horo::XR::OpenXRInternal {
    /** @brief Host-owned exact reference-space borrow; native lifetime remains with the selected space owner. */
    class IOpenXRCompositionSpace {
    public:
        virtual ~IOpenXRCompositionSpace() = default;
        /** @brief Fences exact current Horo space/origin before exposing a synchronous native borrow. */
        [[nodiscard]] virtual Result<XrSpace> Borrow(const XRCoordinateSpace &space) const = 0;
    };

    /** @brief Explicit linear compositor color interpretation; texture transfer encoding remains in the target format. */
    enum class NativeCompositorColorSpace : std::uint8_t {
        RuntimeDefault,
        Rec709,
        Rec2020,
        Count
    };

    /** @brief Finite host-selected renderer budgets in the runtime prediction clock domain. */
    struct NativeFramePacingPolicy final {
        std::int64_t gpuBudgetNanoseconds{1'000'000};
        std::int64_t compositorMarginNanoseconds{500'000};
        std::uint32_t maximumFramesInFlight{1}; /**< This release-complete adapter admits exactly one frame. */
    };

    /** @brief Observations never sleep, perform I/O, poll GPU work or manufacture native completion. */
    enum class NativeFrameDeadline : std::uint8_t {
        WithinBudget,
        CpuSubmissionMissed,
        DisplayMissed
    };

    /** @brief Bounded timing evidence; runtime period is preserved rather than assuming a refresh rate. */
    struct NativeFramePacingObservation final {
        XRFrameId frame;
        XRRenderPredictionTime predictedDisplayTime;
        std::int64_t predictedPeriodNanoseconds{};
        std::int64_t cpuSubmissionDeadlineNanoseconds{};
        std::int64_t remainingNanoseconds{};
        NativeFrameDeadline deadline{NativeFrameDeadline::WithinBudget};
        std::uint32_t framesInFlight{};
    };

    /** @brief Native owners matching each plan target; no renderer or native allocation ownership transfers. */
    struct NativeProjectionTarget final {
        OpenXRSwapchain *owner{};
        XRSwapchainImageId image;
    };

    /** @brief Explicit normalized depth range and matching metric clip planes used to render every submitted view. */
    struct NativeProjectionDepth final {
        float minimumDepth{0};
        float maximumDepth{1};
        float nearMeters{0.1F};
        float farMeters{1000};
    };

    /**
     * @brief One control-thread native frame adapter borrowing the existing logical frame and native allocation owners.
     *
     * The host binds an already started native session and configured XRFrameLifecycle; this class does not start
     * sessions, select renderers, acquire images, record rendering or own GPU completion. Wait/Begin/End record actual
     * native completions in that gate. Projection submission consumes a complete XRViewRenderPlan and requires exact
     * swapchain releases after Renderer retirement. No extra pacing wait or normal-frame device/queue wait is added.
     * Native failure after successful wait retains frame debt; only host-proved quiescent recovery may discard it.
     * All borrowed owners and dispatch loader outlive this immovable adapter. Methods cannot race or be re-entered.
     */
    class OpenXRFrameCompositor final {
    public:
        /** @brief Borrows exact owners; no native work is performed. */
        OpenXRFrameCompositor(OpenXRNativeSession &session, XRFrameLifecycle &frames, IOpenXRCompositionSpace &spaces) noexcept;
        /** @brief Requires explicit native end or quiescent recovery before destruction. */
        ~OpenXRFrameCompositor();
        OpenXRFrameCompositor(const OpenXRFrameCompositor &) = delete;
        OpenXRFrameCompositor &operator=(const OpenXRFrameCompositor &) = delete;

        /** @brief Admits budgets/color policy and resolves official dispatch while no frame is open.
         * @param dispatch Official loader dispatch retained by session. @param pacing Finite supported queue policy.
         * @param colorSpace Explicit runtime compositor primaries, requiring XR_FB_color_space for non-default.
         * @return Success or typed unsupported/invalid/native failure without publishing a partial adapter. */
        [[nodiscard]] Result<void> Bind(NativeOpenXRDispatch dispatch, const NativeFramePacingPolicy &pacing,
                                        NativeCompositorColorSpace colorSpace = NativeCompositorColorSpace::RuntimeDefault);
        /** @brief Reserves before native wait and preserves every successful native wait obligation.
         * @return Frame status plus exact debt identity, or native wait failure after cancelling only its reservation. */
        [[nodiscard]] Result<XRFrameWaitOutcome> Wait();
        /** @brief Executes native begin only for the exact waited frame, then records its completion.
         * @param frame Exact retained native-wait debt. @return Success or typed stale/order/native failure. */
        [[nodiscard]] Result<void> Begin(const XRFrameId &frame);
        /** @brief Observes finite renderer budgets using explicit prediction-domain clock conversion from the host.
         * @param frame Exact outstanding frame. @param now Current time converted into the same runtime prediction domain.
         * @return Bounded timing/deadline observation or stale/invalid input; late frames retain cleanup obligations. */
        [[nodiscard]] Result<NativeFramePacingObservation> ObservePacing(const XRFrameId &frame, XRRenderPredictionTime now) const;
        /** @brief Encodes one complete projection layer, optional complete depth and exact released image generations.
         * @param frame Exact begun frame. @param plan Immutable rendered view/target intent.
         * @param targets Exact released allocation owners, one per unique image.
         * @param depth Complete optional depth convention; required iff plan has depth targets.
         * @return Native handoff result; validation failure issues no native end and preserves the transaction. */
        [[nodiscard]] Result<void> EndProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                 std::span<const NativeProjectionTarget> targets,
                                                 const NativeProjectionDepth *depth = nullptr);
        /** @brief Ends runtime-disabled or explicitly aborted work with no layers after all image releases.
         * @param frame Exact begun frame. @return Native end result; failure retains debt for quiescent recovery. */
        [[nodiscard]] Result<void> EndEmpty(const XRFrameId &frame);
        /** @brief Observes the original terminal native frame fault without native work or admission changes.
         * @return Borrowed fault on the creating lane, otherwise null. The observation expires at quiescent reset.
         * @pre Serialized host control; the caller retains this compositor through the synchronous observation.
         */
        [[nodiscard]] const Error *RetainedFault() const noexcept;
        /** @brief Closes new waits while retaining native frame and image release obligations.
         * @return Success or a typed wrong-thread failure without mutation. */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Drops dispatch/debt after host-proved native quiescence; the logical gate remains host-owned.
         * @return Success or a typed wrong-thread failure without mutation.
         * @pre The host has proved native quiescence and retired every Renderer/image lease. */
        [[nodiscard]] Result<void> ResetAfterQuiescence();

    private:
        /** @brief Synchronous immutable encoding context; no reference escapes native EndProjection. */
        struct ProjectionEncoding final {
            const XRViewRenderPlan &plan;
            std::span<const NativeProjectionTarget> targets;
            const NativeProjectionDepth *depth;
        };

        /** @brief Preflights the exact complete frame, profile and owner set without native work. */
        [[nodiscard]] Result<void> ValidateProjection(const XRFrameId &frame, const ProjectionEncoding &encoding) const;
        /** @brief Validates complete optional depth intent and negotiated native support. */
        [[nodiscard]] Result<void> ValidateDepthPlan(const ProjectionEncoding &encoding) const;
        /** @brief Encodes one current view and optional depth into caller-owned stable fixed storage. */
        [[nodiscard]] Result<XrSpace> EncodeView(const ProjectionEncoding &encoding, const XRViewDescriptor &view,
                                                 XrCompositionLayerProjectionView &output, XrCompositionLayerDepthInfoKHR &depth) const;
        /** @brief Encodes the exact depth target paired with a color target and admitted conventions. */
        [[nodiscard]] Result<void> EncodeDepth(const ProjectionEncoding &encoding, const XRExternalRenderTargetDescriptor &color,
                                               XrCompositionLayerDepthInfoKHR &output) const;
        [[nodiscard]] Result<NativeSessionBorrow> Current() const;
        [[nodiscard]] Result<void> ValidateFrame(const XRFrameId &frame) const;
        [[nodiscard]] Result<void> EndNative(const XRFrameId &frame, XrEnvironmentBlendMode blend,
                                             const XrCompositionLayerBaseHeader *layer);
        [[nodiscard]] Result<XrSwapchainSubImage> SubImage(const XRExternalRenderTargetDescriptor &target,
                                                           std::span<const NativeProjectionTarget> owners) const;

        OpenXRNativeSession *session_;
        XRFrameLifecycle *frames_;
        IOpenXRCompositionSpace *spaces_;
        const std::thread::id thread_{std::this_thread::get_id()};
        XRSessionId owner_{};
        XRFrameId nativeFrame_{};
        NativeFramePacingPolicy policy_{};
        std::int64_t period_{};
        XrTime nativeDisplayTime_{}; /**< Exact wait result echoed only to native end, including unusable Horo prediction evidence. */
        PFN_xrWaitFrame wait_{};
        PFN_xrBeginFrame begin_{};
        PFN_xrEndFrame end_{};
        bool begun_{};
        bool closing_{};
        std::optional<Error> fault_;
    };
}  // namespace Horo::XR::OpenXRInternal
