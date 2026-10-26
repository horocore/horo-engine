#pragma once
/** @file OpenXRSessionEvents.h
 * @brief Private official event pump for the same native session and canonical lifecycle/frame owners.
 */
#include "Horo/XR/XRFrameLifecycle.h"
#include "OpenXRSwapchain.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief Actual runtime notifications which require an explicit host lifecycle boundary before more sampling. */
    struct NativeSessionEventObservation final {
        bool morePending{};
        bool bindingsChanged{};
        bool originChanged{};
        bool exitRequested{};
    };

    /** @brief Single-control-lane native protocol adapter; never issues a Horo session or frame identity.
     * @details Bind resolves official dispatch against the already activated SAME native session. Poll consumes
     * at most 64 runtime events per App event phase. Only successful xrBeginSession grants Started to the
     * canonical lifecycle. STOPPING closes admission immediately; xrEndSession waits for the SAME frame gate
     * to become Idle and every retained swapchain to have no acquisition. No CPU/GPU wait or clock is added.
     * Native session, lifecycle, frame gate, swapchain owners and loader outlive this adapter and all calls.
     */
    class OpenXRSessionEvents final {
    public:
        /** @brief Borrows existing authorities; construction does not resolve or call native functions. */
        OpenXRSessionEvents(OpenXRNativeSession &native, XRSessionLifecycle &sessions, XRFrameLifecycle &frames) noexcept;
        /** @brief Resolves exact official dispatch and copies bounded borrowed swapchain owners outside frame work.
         * @param getProc Official resolver from the retained instance's admitted loader.
         * @param swapchains All native image owners which may hold renderer/GUI retirement debt for this session.
         * @return Success or original native/currentness/capacity failure; failed binding publishes no dispatch.
         * @pre Same control lane; no prior binding. The canonical lifecycle already published the exact session.
         */
        [[nodiscard]] Result<void> Bind(PFN_xrGetInstanceProcAddr getProc, std::span<OpenXRSwapchain *const> swapchains);
        /** @brief Consumes bounded official events and progresses legal native session begin/exit/end.
         * @param wantsPlay Existing typed Play owner reduced only to whether it requests an active native session.
         * @return Observed notifications or original failure; no event is synthesized to make a session available.
         * @post Profile/origin notifications require host invalidation before Input/rendering can continue.
         * Lost-event evidence immediately revokes canonical session admission without destroying retained native
         * frame/image obligations or reporting successful native end. Recovery remains an explicit host boundary.
         */
        [[nodiscard]] Result<NativeSessionEventObservation> Poll(bool wantsPlay);
        /** @brief Borrows no state and performs no native work. @return Whether successful native begin still requires end/retirement. */
        [[nodiscard]] bool Running() const noexcept;

    private:
        [[nodiscard]] Result<NativeSessionBorrow> Current() const;
        [[nodiscard]] Result<void> StateChanged(const XrEventDataSessionStateChanged &event);
        [[nodiscard]] Result<void> Progress(bool wantsPlay);
        /** @brief Applies one fenced native event through the same canonical lifecycle and accumulates value-only notices.
         * @param buffer Actual native event. @param session Same current native session. @param observation Bounded poll result.
         * @return Original lifecycle/event-loss error, retaining native cleanup debt.
         */
        [[nodiscard]] Result<void> ProcessEvent(const XrEventDataBuffer &buffer, XrSession session,
                                                NativeSessionEventObservation &observation);

        [[nodiscard]] bool ImagesRetired() const noexcept;
        [[nodiscard]] static Error Failure(const char *operation, XrResult result);

        struct Dispatch final {
            PFN_xrPollEvent poll{};
            PFN_xrBeginSession begin{};
            PFN_xrEndSession end{};
            PFN_xrRequestExitSession requestExit{};
        } dispatch_;

        OpenXRNativeSession *native_;
        XRSessionLifecycle *sessions_;
        XRFrameLifecycle *frames_;
        const std::thread::id lane_{std::this_thread::get_id()};
        XRSessionId session_;
        std::uint64_t incarnation_{};
        std::array<OpenXRSwapchain *, XRFrameHardLimits::MaximumImages> swapchains_{};
        std::size_t swapchainCount_{};
        XrSessionState observed_{XR_SESSION_STATE_UNKNOWN};
        bool running_{};
        bool exitRequested_{};
        bool dispatching_{}; /**< Owner-lane reentry guard, including official resolver/native callbacks. */
    };
}  // namespace Horo::XR::OpenXRInternal
