#pragma once

/** @file OpenXRFrameCompositor.h
 * @brief Non-installed native predicted-frame, projection/depth handoff and bounded pacing adapter.
 */

#include "Horo/XR/XRFrameLifecycle.h"
#include "Horo/XR/XRPassthrough.h"
#include "OpenXRFrameTelemetry.h"
#include "OpenXRSwapchain.h"

#include <array>
#include <chrono>
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

    /** @brief Selects a bounded projection rectangle scale at frame admission using exact completed GPU work. */
    class NativeProjectionResolution final {
    public:
        static constexpr std::uint32_t FullPercent = 100;

        /** @brief Selects the scale for this frame and resets evidence across owner or capability replacement. */
        [[nodiscard]] std::uint32_t BeginFrame(const XRFrameId &frame, bool adaptive, std::uint64_t deviceIncarnation,
                                               std::uint64_t rendererRevision) noexcept;
        /** @brief True only for the current eligible frame with no retained completion sample. */
        [[nodiscard]] bool AwaitsCompletion(const XRFrameId &frame) const noexcept;
        /** @brief Compares the exact completed GPU duration with the runtime's predicted frame period.
         * @param observation Current frame identity and runtime period.
         * @param gpuElapsed Positive scene submission duration from the selected renderer.
         * @details Changes apply to the next admitted frame. Invalid or missing evidence cannot change scale. */
        void ObserveCompletion(const NativeFramePacingObservation &observation, std::chrono::nanoseconds gpuElapsed) noexcept;
        /** @brief Converts the admitted scale to an active rectangle without changing the allocation. */
        [[nodiscard]] static XRRenderRectangle Rectangle(Render::FramebufferExtent extent, std::uint32_t percent) noexcept;

    private:
        static constexpr std::uint32_t MinimumPercent = 70;
        static constexpr std::uint32_t StepPercent = 5;
        static constexpr std::uint32_t LateFrames = 3;
        static constexpr std::uint32_t HeadroomFrames = 10;
        XRFrameId frame_;
        std::uint64_t deviceIncarnation_{};
        std::uint64_t rendererRevision_{};
        std::uint32_t scale_{FullPercent};
        std::uint32_t frameScale_{FullPercent};
        std::uint32_t late_{};
        std::uint32_t headroom_{};
        bool completed_{};
    };

    /** @brief Native owners matching each plan target; no renderer or native allocation ownership transfers. */
    struct NativeProjectionTarget final {
        OpenXRSwapchain *owner{};
        XRSwapchainImageId image;
    };

    /** @brief Exact released owners and renderer-produced optional evidence borrowed through synchronous frame end. */
    struct NativeReprojectionSubmission final {
        const NativeReprojectionProvider &provider;
        const OpenXRSwapchain &motion;
        const OpenXRSwapchain &depth;
        const XRReprojectionInputs &inputs;
    };

    /** @brief Same-frame receipt produced only after the selected renderer has rendered premultiplied alpha.
     * @details The host verifies its complete projection target receipt before constructing this private submission.
     */
    struct NativePassthroughProjection final {
        XRFrameId frame;
        XRPassthroughScope scope;
        std::optional<XRPoseSample> referenceToWorld{}; /**< Same prediction's native-reference calibration, when eyes use that space. */
        NativePassthroughAuthorization authorization; /**< Checked before submission and after an accepted native frame end. */
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
                                        NativeCompositorColorSpace colorSpace = NativeCompositorColorSpace::RuntimeDefault,
                                        const NativeFrameMetricHandles &metrics = {});
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
        /** @brief Ends the exact projection once, attaching native synthesis only when all optional evidence is current.
         * @param frame Exact begun frame. @param plan Complete ordinary color projection.
         * @param targets Exact released ordinary owners. @param synthesis Renderer-produced optional inputs.
         * @return The actual optional selection or original frame/native error. Missing optional evidence submits baseline.
         * @pre The selected Renderer produced the motion/depth inputs and resolved invalid/disoccluded pixel policy for the route.
         */
        [[nodiscard]] Result<XRReprojectionStatus> EndReprojectedProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                                            std::span<const NativeProjectionTarget> targets,
                                                                            const NativeReprojectionSubmission &synthesis);
        /** @brief Submits exact consented runtime underlay before a completed premultiplied-alpha projection.
         * @param frame Exact begun frame. @param plan Immutable rendered view/target intent.
         * @param targets Exact released allocation owners, one per unique image.
         * @param passthrough Exact frame/World receipt from the selected renderer and current grant.
         * @param depth Complete optional depth convention, required when the plan has depth targets.
         * @return Native result or typed failure before end, retaining the caller's explicit abort obligation.
         */
        [[nodiscard]] Result<void> EndPassthroughProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                            std::span<const NativeProjectionTarget> targets,
                                                            const NativePassthroughProjection &passthrough,
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

        struct ReprojectionEncoding final {
            std::array<XrFrameSynthesisInfoEXT, XRHardLimits::MaximumViews> ext{};
            std::array<XrCompositionLayerSpaceWarpInfoFB, XRHardLimits::MaximumViews> fb{};
            NativeReprojectionRoute route{NativeReprojectionRoute::None};
            XrSession session{XR_NULL_HANDLE};
            PFN_xrEndFrame end{};
        };

        [[nodiscard]] Result<bool> EndEncodedProjection(const XRFrameId &frame, const ProjectionEncoding &encoding,
                                                        const NativeReprojectionSubmission *synthesis,
                                                        const NativePassthroughProjection *passthrough = nullptr);
        [[nodiscard]] std::optional<ReprojectionEncoding> EncodeReprojection(const NativeReprojectionSubmission &synthesis) const;
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
        [[nodiscard]] Result<void> EndNative(const XRFrameId &frame, XrSession session, XrEnvironmentBlendMode blend,
                                             const XrCompositionLayerBaseHeader *layer, const XRViewRenderPlan *projection,
                                             PFN_xrEndFrame dispatch, const NativePassthroughProjection *passthrough = nullptr);
        [[nodiscard]] Result<XrSwapchainSubImage> SubImage(const XRExternalRenderTargetDescriptor &target,
                                                           std::span<const NativeProjectionTarget> owners) const;

        /** @brief Emits only a closed native result, without session or tracking identifiers. */
        void RecordOutcome(NativeFrameMetricOutcome outcome) const noexcept;

        OpenXRNativeSession *session_;
        XRFrameLifecycle *frames_;
        IOpenXRCompositionSpace *spaces_;
        const std::thread::id thread_{std::this_thread::get_id()};
        XRSessionId owner_{};
        XRFrameId nativeFrame_{};
        NativeFramePacingPolicy policy_{};
        NativeFrameMetricHandles metrics_{};
        std::int64_t period_{};
        XrTime nativeDisplayTime_{}; /**< Exact wait result echoed only to native end, including unusable Horo prediction evidence. */
        PFN_xrWaitFrame wait_{};
        PFN_xrBeginFrame begin_{};
        PFN_xrEndFrame end_{};
        bool begun_{};
        bool closing_{};
        std::optional<Error> fault_;
    };

    /** @brief Renderer inputs issued only when both native imports belong to one exact frame attempt. */
    struct NativeReprojectionFramePair final {
        XRFrameId frame;
        NativeReprojectionAcquisition motion;
        NativeReprojectionAcquisition depth;
    };

    /** @brief Control-lane coordinator for the selected provider's two already-created swapchains.
     * @details Partial acquisitions are retained separately from XRFrameLifecycle so an ordinary
     * projection can end while the unreferenced optional swapchains still owe wait/release.
     * The host retains both swapchains through normal retirement or their exact session-loss close.
     */
    class OpenXRReprojectionFrameImages final {
    public:
        /** @brief Borrows the canonical frame and both live native image owners through retirement. */
        OpenXRReprojectionFrameImages(XRFrameLifecycle &frames, OpenXRSwapchain &motion, OpenXRSwapchain &depth) noexcept;
        OpenXRReprojectionFrameImages(const OpenXRReprojectionFrameImages &) = delete;
        OpenXRReprojectionFrameImages &operator=(const OpenXRReprojectionFrameImages &) = delete;

        /** @brief Makes one zero-wait optional acquire attempt for the current projection.
         * @param frame Exact current frame.
         * @param projection Ordinary images already acquired in canonical order.
         * @return Both usable imports, no pair for ordinary fallback, or original failure with cleanup retained.
         * @post No auxiliary image is registered with the canonical frame.
         */
        [[nodiscard]] Result<std::optional<NativeReprojectionFramePair>> Prepare(const XRFrameId &frame,
                                                                                 std::span<const XRSwapchainImageId> projection);
        /** @brief Commits both current auxiliary images only after the same Renderer submission accepted them and color.
         * @param frame Exact prepared frame.
         * @param projection Ordinary images accepted by the same Renderer submission.
         * @return Success or typed validation failure leaving the prepared debt available for abort/retirement.
         */
        [[nodiscard]] Result<void> Submit(const XRFrameId &frame, std::span<const XRSwapchainImageId> projection);
        /** @brief Revokes the exact prepared pair when the host chooses ordinary projection or aborts.
         * @param frame Same prepared frame. @return Success or stale/order error with no mutation.
         */
        [[nodiscard]] Result<void> Omit(const XRFrameId &frame);
        /** @brief Advances the two wait/retire/release debts without a queue wait.
         * @return True when both debts are retired, false while pending, or original failure with debt retained.
         * @post Only actually released, registered images advance XRFrameLifecycle.
         */
        [[nodiscard]] Result<bool> PollRetirement();

    private:
        enum class Phase : std::uint8_t {
            Idle,
            Prepared,
            Omitted,
            Submitted
        };
        [[nodiscard]] Result<void> ValidateOwners(const XRFrameId &frame) const;
        [[nodiscard]] Result<bool> Release(OpenXRSwapchain &owner, const XRSwapchainImageId *registered);

        XRFrameLifecycle *frames_;
        std::array<OpenXRSwapchain *, 2> owners_;
        XRFrameId attempted_;
        std::optional<NativeReprojectionFramePair> pair_;
        std::array<bool, 2> released_{};
        Phase phase_{Phase::Idle};
    };
}  // namespace Horo::XR::OpenXRInternal
