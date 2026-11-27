#pragma once
/** @file OpenXRInputClock.h
 * @brief Application cutoff conversion between actual native OS evidence and the sole scheduler's elapsed simulation time.
 */
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/XR/XRSpacePose.h"
#include "OpenXRNativeSession.h"

namespace Horo::Application::Internal {
    /** @brief Timestamp in a monotonic clock domain supplied by the application. */
    struct OpenXRInputCaptureStamp final {
        std::uint64_t clockDomain{};
        std::uint64_t nanoseconds{};
        std::uint64_t captureGeneration{};
    };

    /** @brief Optional read-only capture-clock port supplied by the application. */
    struct OpenXRInputCaptureClock final {
        const void *owner{};
        std::optional<OpenXRInputCaptureStamp> (*sample)(const void *owner) noexcept {};

        /** @brief Reads the current stamp without retaining or opening the capture owner. */
        [[nodiscard]] std::optional<OpenXRInputCaptureStamp> Now() const noexcept;
    };

    /** @brief Bounds one XR time observation in the listening operation's Audio clock domain. */
    struct OpenXRInputCaptureInterval final {
        OpenXRInputCaptureStamp earliest;
        OpenXRInputCaptureStamp latest;

        /** @brief Bounds an observation, provided both samples are ordered and from the same capture lease. */
        [[nodiscard]] static std::optional<OpenXRInputCaptureInterval> Between(
            const std::optional<OpenXRInputCaptureStamp> &earliest, const std::optional<OpenXRInputCaptureStamp> &latest) noexcept;
    };

    /** @brief Optional clock intervals for two separately sampled facts within one committed Input cutoff. */
    struct OpenXRInputCaptureWindows final {
        std::optional<OpenXRInputCaptureInterval> poses;
        std::optional<OpenXRInputCaptureInterval> actions;
    };

    /** @brief Exact Input cutoff and optionally a measurement of that same sample in the Audio clock. */
    struct OpenXRInputClockObservation final {
        XR::XRTimeEvidence time;
        std::optional<OpenXRInputCaptureInterval> captureInterval;
    };

    /** @brief Returns the single platform time route shared by cold discovery, preflight and actual runtime sampling. */
    [[nodiscard]] constexpr XR::OpenXRInternal::NativeFeatureProviderRoute OpenXRPlatformTimeRoute() noexcept {
#if defined(_WIN32)
        return {"XR_KHR_win32_convert_performance_counter_time", "xrConvertWin32PerformanceCounterToTimeKHR"};
#else
        return {"XR_KHR_convert_timespec_time", "xrConvertTimespecTimeToTimeKHR"};
#endif
    }

    /** @brief One same-owner native conversion bound to the existing scheduler, never a clock or tick issuer.
     * @details Bind is cold composition. Read accepts only the SAME scheduler's active BuildInputSnapshot dispatch,
     * uses its committed tick and immutable fixed step, samples the actual platform native counter and invokes
     * the enabled conversion extension on the SAME admitted instance. No guessed epoch, offset, +1ns or presentation
     * prediction participates. Native owner, scheduler and loader outlive every call; all methods use their owner lane.
     */
    class OpenXRInputClock final {
    public:
        /** @brief Borrows the native parent; no native operation or allocation is performed. */
        explicit OpenXRInputClock(XR::OpenXRInternal::OpenXRNativeSession &native) noexcept;
        OpenXRInputClock(const OpenXRInputClock &) = delete;
        OpenXRInputClock &operator=(const OpenXRInputClock &) = delete;
        /** @brief Binds actual successful native/session and scheduler identities before interactive input.
         * @param source SAME RuntimeHost dispatch issuer, bound while it is idle.
         * @param fixedStep SAME immutable config value supplied to RuntimeHost::Create.
         * @return Success or original admission/native failure; failed binding leaves Read unavailable.
         */
        [[nodiscard]] Result<void> Bind(Runtime::RuntimeDispatchSource source, Duration fixedStep);
        /** @brief Seals the SAME idle scheduler/config during actual App composition before ScreenStart.
         * @param source Canonical existing RuntimeHost issuer, idle on its owner lane.
         * @param fixedStep Exact immutable config supplied at its creation.
         * @return Typed failure without mutation, or a sealed source; no native session is required yet.
         */
        [[nodiscard]] Result<void> PrepareScheduler(Runtime::RuntimeDispatchSource source, Duration fixedStep);
        /** @brief Binds the actual issued native session during the canonical Scene/resource preparation stage.
         * @return Original native/currentness failure; scheduler identity/config cannot be replaced here.
         * @pre PrepareScheduler succeeded while idle. This stage may occur inside that SAME scheduler's Scene phase;
         * it authorizes no cutoff by itself. Read still requires exact opaque BuildInputSnapshot evidence.
         */
        [[nodiscard]] Result<void> BindNative();
        /** @brief Drops one exact drained native binding while preserving the SAME scheduler and simulation epoch.
         * @param session Exact previously bound candidate; foreign/stale identities cannot unbind a replacement.
         * @return Success or typed owner/reentry/stale failure, with no native call or tick/config mutation.
         * @pre The host has physically retired native/frame/image obligations, before replacing their parent session.
         */
        [[nodiscard]] Result<void> RetireNative(const XR::XRSessionId &session);
        /** @brief Converts one actual Input cutoff with checked multiplication and native-instance fences.
         * @param context Actual scheduler callback; numeric fields alone cannot authorize this read.
         * @return Independent native runtime sample plus exact committed simulation time, or typed stale/overflow/native failure.
         * @post No Input commit, snapshot publication, tick increment, per-frame allocation or new epoch.
         */
        [[nodiscard]] Result<XR::XRTimeEvidence> Read(const Runtime::FrameContext &context) const;
        /** @brief Reads the Input cutoff and brackets its native sample with the supplied capture clock.
         * @param context Actual BuildInputSnapshot dispatch on the owner lane.
         * @param capture Borrowed clock port. Reads must be bounded and non-blocking.
         * @return Native cutoff with a measured interval when both observations belong to the same ordered Audio clock.
         * @details Missing, switched or regressing clocks and replaced capture generations leave the interval absent.
         * The interval applies to poses located at this cutoff. It does not timestamp actions queried later.
         */
        [[nodiscard]] Result<OpenXRInputClockObservation> ReadWithCaptureClock(const Runtime::FrameContext &context,
                                                                               OpenXRInputCaptureClock capture) const;
        /** @brief Pairs an actual native frame prediction with a fresh same-instance OS-counter conversion.
         * @param prediction Exact xrWaitFrame prediction retained by the canonical frame gate, never a simulation timestamp.
         * @return Runtime sample plus presentation evidence, or the original native/currentness error.
         * @post No simulation epoch/tick or Input publication is read or mutated. The caller must still fence its exact frame.
         */
        [[nodiscard]] Result<XR::XRTimeEvidence> PresentationTime(XR::XRRenderPredictionTime prediction) const;
        /** @brief Samples the same enabled native conversion as an explicit presentation-domain deadline timestamp.
         * @return Current official XrTime converted to the prediction domain, or the original conversion/currentness error.
         * @details xrWaitFrame predictions and the enabled timespec/QPC conversion both use the SAME instance's XrTime
         * nanoseconds. This boundary changes only the purpose type; it never converts Foundation or simulation epochs.
         * No tick, offset, guessed refresh interval, wait or Input publication is introduced.
         */
        [[nodiscard]] Result<XR::XRRenderPredictionTime> PresentationNow() const;
        /** @brief Samples the exact admitted instance clock without issuing a simulation or prediction cutoff.
         * @return Native runtime time or the original lane, provider or conversion failure.
         * @post No simulation cutoff, display prediction, frame wait or pose publication is created.
         */
        [[nodiscard]] Result<XR::XRRuntimeTime> RuntimeNow() const;
        /** @brief Reads the same native runtime time after the prior session has physically retired.
         * @param attempt Exact still-prepared loader instance and system.
         * @return Current runtime time or original lane, route, dispatch, conversion or lifetime failure.
         * @details Cold replacement admission only. This grants no frame, pose or Input cutoff authority.
         */
        [[nodiscard]] Result<XR::XRRuntimeTime> ReadRetiredSessionTime(XR::XRLoaderPreflightAttempt attempt) const;

    private:
        /** @brief Samples actual OS evidence through the same retained native conversion under the owner/reentry fence.
         * @return Positive same-instance XrTime, or original lane/currentness/native error without publication.
         */
        [[nodiscard]] Result<XR::XRRuntimeTime> PresentationSample() const;
        [[nodiscard]] static Result<XR::XRRuntimeTime> NativeSample(XrInstance instance, PFN_xrVoidFunction convert);
        XR::OpenXRInternal::OpenXRNativeSession *native_;
        Runtime::RuntimeDispatchSource source_;
        Duration fixedStep_;
        XR::XRSessionId session_;
        std::optional<XR::OpenXRInternal::NativeFeatureProviderHandle> provider_;
        const std::thread::id lane_{std::this_thread::get_id()};
        mutable bool reading_{};
    };
}  // namespace Horo::Application::Internal
