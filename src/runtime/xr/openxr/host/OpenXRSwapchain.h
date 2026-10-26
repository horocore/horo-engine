#pragma once

/** @file OpenXRSwapchain.h
 * @brief Non-installed, control-thread OpenXR swapchain ownership and renderer handoff.
 */

#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/XR/XRViewRenderPlan.h"
#include "OpenXRNativeSession.h"

#include <bit>
#include <thread>
#include <type_traits>
#include <utility>

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

    /** @brief Synchronous, borrowed native enumeration context; adapters must not retain it. */
    class IOpenXRSwapchainImageEnumeration {
    public:
        virtual ~IOpenXRSwapchainImageEnumeration() = default;
        /** @brief Admits the bound swapchain and callable before any native output write. */
        [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;
        /** @brief Calls the bound dispatch with the unchanged official output prototype and result.
         * @param capacity Native image capacity. @param written Native count output.
         * @param images API-typed image storage adapted according to the OpenXR graphics binding contract. */
        [[nodiscard]] virtual XrResult Enumerate(std::uint32_t capacity, std::uint32_t *written,
                                                 XrSwapchainImageBaseHeader *images) const = 0;
    };

    /** @brief Allocation-free context binding one swapchain to any borrowed native-signature callable.
     * @details The callable and native allocation outlive this synchronous context. No function pointer
     * is retyped, no callable is copied, and foreign exceptions keep the caller's existing rollback boundary. */
    template <typename Enumerator> class OpenXRSwapchainImageEnumeration final : public IOpenXRSwapchainImageEnumeration {
    public:
        OpenXRSwapchainImageEnumeration(XrSwapchain swapchain, Enumerator &enumerate) noexcept
            : swapchain_(swapchain), enumerate_(enumerate) {}

        [[nodiscard]] bool IsAvailable() const noexcept override {
            if (swapchain_ == XR_NULL_HANDLE)
                return false;
            if constexpr (std::is_null_pointer_v<Enumerator>)
                return false;
            else if constexpr (std::is_pointer_v<Enumerator>)
                return enumerate_ != nullptr;
            else
                return true;
        }

        [[nodiscard]] XrResult Enumerate(std::uint32_t capacity, std::uint32_t *written,
                                         XrSwapchainImageBaseHeader *images) const override {
            if constexpr (std::is_null_pointer_v<Enumerator>) {
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            } else {
                static_assert(std::is_same_v<
                              std::invoke_result_t<Enumerator &, XrSwapchain, std::uint32_t, std::uint32_t *, XrSwapchainImageBaseHeader *>,
                              XrResult>);
                if (!IsAvailable())
                    return XR_ERROR_FUNCTION_UNSUPPORTED;
                return enumerate_(swapchain_, capacity, written, images);
            }
        }

    private:
        XrSwapchain swapchain_;
        Enumerator &enumerate_;
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
        /** @brief Enumerates API-typed images and cold-prepares views using the exact immutable allocation request.
         * @param request Same validated request passed to native swapchain creation; view policy cannot change per acquisition.
         * @param count Exact bounded native image count.
         * @param enumeration Synchronous official dispatch borrow, never retained.
         * @return Prepared images/views or original native/admission failure.
         * @post Failure retains no renderer imports. Successful prefix views remain owned until Reset; the owner
         * keeps the native swapchain and selected device alive until that reset succeeds. */
        [[nodiscard]] virtual Result<void> Enumerate(const NativeSwapchainRequest &request, std::uint32_t count,
                                                     const IOpenXRSwapchainImageEnumeration &enumeration) = 0;
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

    /** @brief Synchronous compositor borrow, valid only until the next acquisition or Close. */
    struct NativeReleasedSwapchainImage final {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        NativeSwapchainRequest allocation;
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
        template <typename Resolver> [[nodiscard]] Result<void> Create(const NativeSwapchainRequest &request, Resolver getProc) {
            if (auto thread = CheckThread(); thread.HasError())
                return thread;
            if constexpr (std::is_null_pointer_v<Resolver>) {
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            } else {
                if constexpr (requires { static_cast<bool>(getProc); }) {
                    if (!getProc)
                        return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
                }
                auto session = AdmitCreate(request);
                if (session.HasError())
                    return Result<void>::Failure(std::move(session).ErrorValue());
                if (auto dispatch = Resolve(getProc, session.Value().instance); dispatch.HasError())
                    return dispatch;
                request_ = request;
                lastTarget_ = request.target;
                closing_ = false;
                lost_ = false;
                malformedAcquisition_ = false;
                try {
                    auto prepared = Prepare(session.Value());
                    if (prepared.HasValue())
                        return prepared;
                    // Keep the original preparation diagnostic; failed cleanup remains retained for Close retry.
                    if (const auto rollback = Close(); rollback.HasError())
                        return Result<void>::Failure(rollback.ErrorValue());
                    return prepared;
                } catch (...) {
                    const auto rollback = Close();
                    (void)rollback;
                    throw;
                }
            }
        }

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
        /** @brief Borrows only the latest successfully released acquisition for native compositor encoding.
         * @param image Exact image generation released after Renderer retirement.
         * @return Native swapchain and allocation or stale/unavailable; no ownership is transferred.
         * @pre The host serializes this borrow and xrEndFrame before the next acquisition or Close. */
        [[nodiscard]] Result<NativeReleasedSwapchainImage> BorrowReleased(const XRSwapchainImageId &image) const;
        /** @brief Returns lifecycle evidence without native calls. @return Current retained ownership phase. */
        [[nodiscard]] NativeSwapchainPhase Phase() const noexcept;

    private:
        [[nodiscard]] Result<void> CheckThread() const;
        [[nodiscard]] Result<void> CheckCurrent() const;
        [[nodiscard]] Result<NativeSessionBorrow> AdmitCreate(const NativeSwapchainRequest &request) const;
        [[nodiscard]] static Error DispatchFailure(const char *name, XrResult result);

        /** @brief Secures candidate-scoped dispatch before native allocation. */
        template <typename Function, typename Resolver>
        [[nodiscard]] static Result<void> Load(Resolver &getProc, XrInstance instance, const char *name, Function &function) {
            static_assert(std::is_function_v<std::remove_pointer_t<Function>>);
            static_assert(std::is_same_v<std::invoke_result_t<Resolver &, XrInstance, const char *, PFN_xrVoidFunction *>, XrResult>);
            PFN_xrVoidFunction raw{};
            function = {};
            if (const XrResult result = getProc(instance, name, &raw); XR_FAILED(result) || !raw)
                return Result<void>::Failure(DispatchFailure(name, XR_FAILED(result) ? result : XR_ERROR_FUNCTION_UNSUPPORTED));
            function = std::bit_cast<Function>(raw);
            return Result<void>::Success();
        }

        template <typename Resolver> [[nodiscard]] Result<void> Resolve(Resolver &getProc, XrInstance instance) {
            if (auto r = Load(getProc, instance, "xrEnumerateSwapchainFormats", dispatch_.formats); r.HasError())
                return r;
            if (auto r = Load(getProc, instance, "xrCreateSwapchain", dispatch_.create); r.HasError())
                return r;
            if (auto r = Load(getProc, instance, "xrDestroySwapchain", dispatch_.destroy); r.HasError())
                return r;
            if (auto r = Load(getProc, instance, "xrEnumerateSwapchainImages", dispatch_.enumerate); r.HasError())
                return r;
            if (auto r = Load(getProc, instance, "xrAcquireSwapchainImage", dispatch_.acquire); r.HasError())
                return r;
            if (auto r = Load(getProc, instance, "xrWaitSwapchainImage", dispatch_.wait); r.HasError())
                return r;
            return Load(getProc, instance, "xrReleaseSwapchainImage", dispatch_.release);
        }

        /** @brief Selects the exact requested format from bounded native discovery before allocation.
         * @param session Native session from the current fenced owner. @return Exact native format or original validation failure. */
        [[nodiscard]] Result<std::int64_t> SelectRuntimeFormat(XrSession session) const;
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
        };

        Dispatch dispatch_;

        OpenXRNativeSession *session_;
        IOpenXRSwapchainImages *images_;
        const std::thread::id thread_{std::this_thread::get_id()};
        NativeSwapchainRequest request_{};
        XRSwapchainTargetId lastTarget_{};
        XrSwapchain native_{XR_NULL_HANDLE};
        NativeSwapchainPhase phase_{NativeSwapchainPhase::Empty};
        NativeSwapchainAcquisition acquisition_{};
        XRSwapchainImageId releasedImage_{};
        std::uint32_t count_{};
        std::uint32_t generation_{};
        std::uint32_t index_{};
        bool closing_{};
        bool malformedAcquisition_{};
        bool lost_{};
        bool retireRequested_{};
    };
}  // namespace Horo::XR::OpenXRInternal
