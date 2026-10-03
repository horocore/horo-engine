#pragma once

/**
 * @file XRFrameLifecycle.h
 * @brief Bounded, backend-neutral predicted-frame transaction owned by XRRuntime.
 */

#include "Horo/XR/XRSessionLifecycle.h"
#include "Horo/XR/XRSpacePose.h"

#include <array>
#include <compare>
#include <cstdint>
#include <span>

namespace Horo::XR {
    /** @brief Fixed implementation ceilings, not an admitted product or runtime capability. */
    struct XRFrameHardLimits final {
        static constexpr std::uint32_t MaximumViews = XRHardLimits::MaximumViews;
        static constexpr std::uint32_t MaximumImages = MaximumViews * 2U;
        static constexpr std::uint32_t MaximumLayers = 16;
    };

    /** @brief One exact frame under its session, configuration, and capability publication. */
    struct XRFrameId final {
        XRSessionId session;                     /**< Exact runtime/session incarnation. */
        XRViewConfigurationId configuration;     /**< Accepted view-configuration incarnation. */
        XRCapabilityRevision capabilityRevision; /**< Captured capability-plan evidence. */
        std::uint64_t sequence{};                /**< Non-zero, non-reused coordinator sequence. */

        /** @brief Checks representation, not currentness. @return True when every owner and sequence is present. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return session.IsValid() && configuration.IsValid() && configuration.session == session && capabilityRevision.IsValid() &&
                   sequence != 0;
        }

        constexpr auto operator<=>(const XRFrameId &) const noexcept = default;
    };

    /** @brief Exact transaction phase; native calls remain with the selected XROpenXR adapter. */
    enum class XRFramePhase : std::uint8_t {
        Idle,
        WaitReserved,
        Waited,
        Begun,
        ViewsLocated,
        ImagesAcquired,
        RendererSubmitted,
        Aborting,
        ImagesReleased
    };

    /** @brief Allocation-free and actionable frame admission/ordering outcome. */
    enum class XRFrameStatus : std::uint8_t {
        Ok,
        InvalidInput,
        Unsupported,
        Unavailable,
        StaleSession,
        StalePlan,
        StaleFrame,
        CapacityExceeded,
        Duplicate,
        OutOfOrder,
        ImageNotAcquired,
        ImageNotReleased,
        IncompleteSubmission,
        SequenceExhausted,
        Shutdown
    };

    /** @brief Immutable finite bounds admitted for one session/configuration publication. */
    struct XRFrameLimits final {
        std::uint32_t maximumViews{};  /**< Zero permits only non-rendering frame cycles. */
        std::uint32_t maximumImages{}; /**< Maximum simultaneously acquired runtime images. */
        std::uint32_t maximumLayers{}; /**< Maximum native composition layers at end. */
    };

    /**
     * @brief Typed wait outcome. A failed reservation has no frame; a rejected completed native wait retains a frame
     * that must still be begun and ended with zero layers, or retired after host-proved native quiescence.
     */
    struct XRFrameWaitOutcome final {
        XRFrameStatus status{XRFrameStatus::InvalidInput};
        XRFrameId frame;
    };

    /** @brief Fixed-size publication; no borrowed frame-local storage escapes the coordinator. */
    struct XRFrameSnapshot final {
        XRFrameId frame;
        XRRenderPredictionTime predictedDisplayTime; /**< Exact runtime prediction, not a simulation clock or pacing policy. */
        XRFramePhase phase{XRFramePhase::Idle};
        std::uint32_t locatedViews{};
        std::uint32_t acquiredImages{};
        std::uint32_t releasedImages{};
        bool shouldRender{};   /**< Unmodified runtime intent, even when rendering cannot be admitted. */
        bool renderAdmitted{}; /**< False for a zero-layer native completion obligation. */
    };

    /**
     * @brief Single-control-thread XR frame gate; records completed adapter operations without performing native work.
     *
     * XRSessionLifecycle and the selected backend/Renderer owner outlive this gate. Methods cannot race or be re-entered.
     * The host reserves a frame before native wait, cancels that reservation only when native wait fails, and records
     * every successful native wait even if its prediction or render intent is unusable. Such a completed wait still
     * returns a valid frame to begin/end with zero layers while the native session remains live; the host must never
     * discard it solely because of a non-Ok render status.
     * Single-thread ownership does not itself prevent session mutation between reservation and native wait completion.
     * The host must defer session replacement, loss publication and resource retirement until the native frame is
     * ended, or first prove native quiescence and use ResetAfterQuiescence. If the session changes unexpectedly, the
     * recorded frame remains open but currentness checks fence further native work; only quiescent recovery retires it.
     * The host calls native begin/locate/acquire/submit/release/end at the owning adapter, then records successful
     * completions here in the matching order. It must not perform the next native operation when this gate rejects its
     * precursor. Native failure after begin is resolved through the backend's legal abort/zero-layer path; this gate
     * never manufactures an OpenXR success or GPU completion. ResetAfterQuiescence and Shutdown are legal only after
     * the host has stopped producers, finished/abandoned the native frame and retired acquired external leases.
     * Successful frame-hot methods use fixed storage with no allocation, blocking I/O, or CPU/GPU synchronization.
     */
    class XRFrameLifecycle final {
    public:
        /** @brief Binds the owning session coordinator. @param sessions Owner that outlives this gate. */
        explicit XRFrameLifecycle(XRSessionLifecycle &sessions) noexcept;

        XRFrameLifecycle(const XRFrameLifecycle &) = delete;
        XRFrameLifecycle &operator=(const XRFrameLifecycle &) = delete;

        /**
         * @brief Admit an exact configuration and finite per-frame bounds while no frame is open.
         * @param configuration Currently active configuration owned by the published session.
         * @param capabilities Exact accepted system capability publication; no reference is retained.
         * @param limits Finite runtime/product limits, no greater than the publication or XRFrameHardLimits.
         * @return Ok or typed invalid, stale, capacity, order, or shutdown status without partial mutation.
         */
        [[nodiscard]] XRFrameStatus BindConfiguration(const XRViewConfigurationId &configuration, const XRCapabilitySnapshot &capabilities,
                                                      XRFrameLimits limits) noexcept;

        /**
         * @brief Reserve one exact frame before invoking native wait; reject unavailable/stale/duplicate/exhausted work first.
         * @param session Current session admitted for frames by XRSessionLifecycle.
         * @return Reserved frame or typed rejection with no native wait debt.
         */
        [[nodiscard]] XRFrameWaitOutcome ReserveWait(const XRSessionId &session) noexcept;

        /**
         * @brief Cancel a reserved frame only after native wait failed without producing a frame.
         * @return Ok or OutOfOrder; sequence identity is never reused.
         */
        [[nodiscard]] XRFrameStatus CancelWait() noexcept;

        /**
         * @brief Record the successful native wait against the single reserved frame.
         * @param predictedDisplayTime Runtime prediction; it is never used to pace Renderer here.
         * @param shouldRender Runtime-supplied intent, preserved even when rendering is unsupported.
         * @return A retained frame after a valid reservation; non-Ok render status requires zero-layer begin/end while
         * the native session is live, or host-proved quiescent recovery if that session was retired or lost.
         */
        [[nodiscard]] XRFrameWaitOutcome RecordWait(XRRenderPredictionTime predictedDisplayTime, bool shouldRender) noexcept;

        /** @brief Record native begin for the waited frame. @param frame Exact open frame. @return Typed order/owner result. */
        [[nodiscard]] XRFrameStatus Begin(const XRFrameId &frame) noexcept;

        /**
         * @brief Record complete native view location before any image acquisition.
         * @param frame Exact begun rendering frame.
         * @param viewCount Complete non-zero runtime view count, within admitted limits.
         * @return Typed order, unsupported, invalid, or capacity result.
         */
        [[nodiscard]] XRFrameStatus LocateViews(const XRFrameId &frame, std::uint32_t viewCount) noexcept;

        /**
         * @brief Record one acquired and ready runtime image generation.
         * @param frame Exact located rendering frame.
         * @param image Exact session-owned image; duplicate or over-capacity input leaves state unchanged.
         * @return Typed order, identity, duplicate, or capacity result.
         */
        [[nodiscard]] XRFrameStatus Acquire(const XRFrameId &frame, const XRSwapchainImageId &image) noexcept;

        /**
         * @brief Record complete Renderer submission after the bridge has proved use/completion for every image.
         * @param frame Exact rendering frame with at least one acquired image.
         * @param completedImages Complete acquired image identities in acquisition order.
         * @return Ok or typed incomplete/mismatched/order result; this does not wait for the GPU.
         */
        [[nodiscard]] XRFrameStatus Submit(const XRFrameId &frame, std::span<const XRSwapchainImageId> completedImages) noexcept;

        /**
         * @brief Choose a backend-confirmed legal zero-layer abort after begin, retaining image-release obligations.
         * @param frame Exact begun frame.
         * @return Typed order/owner result; no native abort is performed here.
         */
        [[nodiscard]] XRFrameStatus Abort(const XRFrameId &frame) noexcept;

        /**
         * @brief Record exact native image release after submission or legal abort and completion proof.
         * @param frame Exact open frame.
         * @param image Previously acquired and not-yet-released image.
         * @return Typed owner/order/duplicate/not-acquired result.
         */
        [[nodiscard]] XRFrameStatus Release(const XRFrameId &frame, const XRSwapchainImageId &image) noexcept;

        /**
         * @brief Record native end and close the exact transaction only after all release obligations are met.
         * @param frame Exact begun frame.
         * @param layerCount Finite submitted layers; zero for non-rendering or an explicit abort path.
         * @return Typed order, capacity, or unreleased-image result without partial close on failure.
         */
        [[nodiscard]] XRFrameStatus End(const XRFrameId &frame, std::uint32_t layerCount) noexcept;

        /** @brief Return a fixed-size current publication. @return Copy of frame identity, phase, prediction, and counts. */
        [[nodiscard]] XRFrameSnapshot Snapshot() const noexcept;

        /** @brief Drop old session/configuration evidence only after host-proved native quiescence. */
        void ResetAfterQuiescence() noexcept;

        /** @brief Permanently close frame admission after host-proved native quiescence; idempotent. */
        void Shutdown() noexcept;

    private:
        /** @brief Fence one operation against current session, plan and frame identity without mutation. */
        [[nodiscard]] XRFrameStatus ValidateCurrent(const XRFrameId &frame) const noexcept;
        /** @brief Find an acquired image in fixed storage or return MaximumImages as the missing sentinel. */
        [[nodiscard]] std::uint32_t FindImage(const XRSwapchainImageId &image) const noexcept;
        /** @brief Clear the current logical transaction without issuing native cleanup. */
        void ClearFrame() noexcept;

        XRSessionLifecycle *sessions_;
        XRViewConfigurationId configuration_{};
        XRCapabilityRevision revision_{};
        XRFrameLimits limits_{};
        XRFrameSnapshot current_{};
        XRFrameId lastEnded_{};
        std::array<XRSwapchainImageId, XRFrameHardLimits::MaximumImages> images_{};
        std::array<bool, XRFrameHardLimits::MaximumImages> released_{};
        std::uint64_t nextSequence_{1};
        bool aborted_{};
        bool submitted_{};
        bool shutdown_{};
    };
}  // namespace Horo::XR
