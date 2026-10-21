#pragma once

/** @file OpenXRSwapchain.h
 * @brief Non-installed, control-thread OpenXR swapchain ownership and renderer handoff.
 */

#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/XR/XRViewRenderPlan.h"
#include "OpenXRNativeSession.h"

#include <thread>

namespace Horo::XR::OpenXRInternal {
    /** @brief Native role admitted jointly by runtime extensions and the selected renderer adapter. */
    enum class NativeSwapchainRole : std::uint8_t {
        Color,
        Depth,
        Motion,
        Density,
        Count
    };

    /** @brief Explicit requested foveation mechanism; unsupported required mechanisms never silently become None. */
    enum class NativeSwapchainFoveation : std::uint8_t {
        None,
        Fixed,
        GazeDriven,
        Count
    };

    /** @brief Immutable allocation request; view rectangles/layers belong to the frame's XRViewRenderPlan. */
    struct NativeSwapchainRequest final {
        XRSwapchainTargetId target;
        Render::RenderTextureDescriptor texture;
        NativeSwapchainRole role{NativeSwapchainRole::Color};
        NativeSwapchainFoveation foveation{NativeSwapchainFoveation::None};
    };

    /**
     * @brief Selected backend's private image translator, borrowed by exactly one swapchain owner.
     *
     * This port adds no queue, completion or drain authority. Import publishes a borrowed image in
     * the existing frontend registry. Retire closes graph admission and releases that registration;
     * RetirementComplete observes only the canonical native retirement lease after all graph pins
     * and encoded external-final-state obligations have retired. A submission/timeline declaration
     * alone never satisfies it. Implementations preserve the runtime allocation and never destroy it.
     * The host serializes every call on the render-capable control thread, retains the exact device,
     * and keeps the port/session alive until successful Close. Unsupported roles fail before Create.
     */
    class IOpenXRSwapchainImages {
    public:
        virtual ~IOpenXRSwapchainImages() = default;
        /** @brief Fences exact current session/device and validates the complete format/usage/role tuple. */
        [[nodiscard]] virtual Result<void> Validate(const NativeSwapchainRequest &request) const = 0;
        /** @brief Selects a native format from the exact bounded runtime publication, with no format fallback. */
        [[nodiscard]] virtual Result<std::int64_t> SelectFormat(const NativeSwapchainRequest &request,
                                                                std::span<const std::int64_t> formats) const = 0;
        /** @brief Enumerates API-typed images using official dispatch into adapter-owned bounded storage.
         * @post Failure retains no renderer imports. The owner keeps the swapchain until Reset succeeds. */
        [[nodiscard]] virtual Result<void> Enumerate(XrSwapchain swapchain, std::uint32_t count,
                                                     PFN_xrEnumerateSwapchainImages enumerate) = 0;
        /** @brief Imports one exact ready acquisition into the canonical registry.
         * @post Failure retains no registry or native work; partial asynchronous ownership must return a handle for retirement. */
        [[nodiscard]] virtual Result<Render::RenderTextureHandle> Import(const NativeSwapchainRequest &request,
                                                                         const XRSwapchainImageId &image, std::uint32_t nativeIndex) = 0;
        /** @brief Closes graph admission and logically releases this exact imported registration; retryable on failure. */
        [[nodiscard]] virtual Result<void> Retire(Render::RenderTextureHandle texture) = 0;
        /** @brief Observes canonical retirement and actual encoded external release, without CPU/GPU waits or allocation. */
        [[nodiscard]] virtual Result<bool> RetirementComplete(Render::RenderTextureHandle texture) = 0;
        /** @brief Drops API image borrows only after every import has retired; failure retains them for retry. */
        [[nodiscard]] virtual Result<void> Reset() = 0;
    };

    /** @brief Native state retained across bounded wait timeout, render failure and deferred retirement. */
    enum class NativeSwapchainPhase : std::uint8_t {
        Empty,
        Available,
        Acquired,
        Ready,
        Imported,
        Retiring,
        Closing
    };

    /** @brief Exact ready acquisition publication; invalidated before retirement starts. */
    struct NativeSwapchainAcquisition final {
        XRSwapchainImageId image;
        Render::RenderTextureHandle texture;
    };

    /**
     * @brief One immovable runtime allocation owner; it never owns XR frame or GPU submission state.
     *
     * The host follows XRFrameLifecycle before invoking acquire/wait, records completed image operations
     * there, and resolves abort/end-frame separately. Only one acquisition is open per swapchain; the
     * native image index can recur but its Horo acquisition generation never does. Different replacement
     * targets use separate owners/ports while old imports retire. No method waits for device/queue idle.
     */
    class OpenXRSwapchain final {
    public:
        /** @brief Borrows exact session and backend translator; both must outlive successful Close. */
        OpenXRSwapchain(OpenXRNativeSession &session, IOpenXRSwapchainImages &images) noexcept;
        /** @brief Enforces explicit complete retirement before session/device/port destruction. */
        ~OpenXRSwapchain();
        OpenXRSwapchain(const OpenXRSwapchain &) = delete;
        OpenXRSwapchain &operator=(const OpenXRSwapchain &) = delete;

        /** @brief Resolves official dispatch, selects a complete tuple and creates/enumerates a bounded native candidate.
         * @param request Exact new target and immutable allocation policy. @param getProc Official retained-loader dispatch.
         * @return Success or original typed/native failure; incomplete rollback remains owned for Close retry. */
        [[nodiscard]] Result<void> Create(const NativeSwapchainRequest &request, PFN_xrGetInstanceProcAddr getProc);
        /** @brief Acquires one runtime image; zero native work is allowed when an old acquisition remains open.
         * @return Success or typed failure; successful native acquisition always retains its release obligation. */
        [[nodiscard]] Result<void> Acquire();
        /** @brief Performs one bounded native image-ready wait; timeout preserves the acquired obligation.
         * @param timeout Nanoseconds, in [0, 100ms]; XR_INFINITE_DURATION is rejected. @return True when ready, false on timeout. */
        [[nodiscard]] Result<bool> Wait(XrDuration timeout);
        /** @brief Imports the ready acquisition after fresh session/device admission. @return Exact image/texture publication. */
        [[nodiscard]] Result<NativeSwapchainAcquisition> Import();
        /** @brief Revokes the ready publication and starts canonical renderer retirement, or releases an unimported ready image.
         * @return True after native release, false while retirement is pending, or retryable original failure.
         * @details Does not release a native image that has not successfully completed Wait. */
        [[nodiscard]] Result<bool> PollRelease();
        /** @brief Closes admission and progresses release/destruction without waiting; repeat until true.
         * @return True only when native allocation and image borrows are gone; false while ready-wait/retirement is pending. */
        [[nodiscard]] Result<bool> Close();
        /** @brief Returns the exact currently imported publication, or unavailable after revocation/loss/release. */
        [[nodiscard]] Result<NativeSwapchainAcquisition> Current() const;
        /** @brief Fences one exact ready/imported acquisition before backend import or graph admission.
         * @param image Exact owner-issued identity. @return Success or stale/unavailable without native work. */
        [[nodiscard]] Result<void> ValidateAcquisition(const XRSwapchainImageId &image) const;
        /** @brief Returns lifecycle evidence without native calls. @return Current retained ownership phase. */
        [[nodiscard]] NativeSwapchainPhase Phase() const noexcept;

    private:
        [[nodiscard]] Result<void> CheckThread() const;
        [[nodiscard]] Result<void> CheckCurrent() const;
        [[nodiscard]] Result<void> Resolve(PFN_xrGetInstanceProcAddr getProc, XrInstance instance);
        [[nodiscard]] Result<void> Prepare(const NativeSessionBorrow &session);
        [[nodiscard]] Result<bool> ReleaseReady();
        [[nodiscard]] Result<bool> FinishClose();

        struct Dispatch final {
            PFN_xrEnumerateSwapchainFormats formats{};
            PFN_xrCreateSwapchain create{};
            PFN_xrDestroySwapchain destroy{};
            PFN_xrEnumerateSwapchainImages enumerate{};
            PFN_xrAcquireSwapchainImage acquire{};
            PFN_xrWaitSwapchainImage wait{};
            PFN_xrReleaseSwapchainImage release{};
        } dispatch_;

        OpenXRNativeSession *session_;
        IOpenXRSwapchainImages *images_;
        const std::thread::id thread_{std::this_thread::get_id()};
        NativeSwapchainRequest request_{};
        XRSwapchainTargetId lastTarget_{};
        XrSwapchain native_{XR_NULL_HANDLE};
        NativeSwapchainPhase phase_{NativeSwapchainPhase::Empty};
        NativeSwapchainAcquisition acquisition_{};
        std::uint32_t count_{};
        std::uint32_t generation_{};
        std::uint32_t index_{};
        bool closing_{};
        bool malformedAcquisition_{};
        bool lost_{};
        bool retireRequested_{};
    };
}  // namespace Horo::XR::OpenXRInternal
