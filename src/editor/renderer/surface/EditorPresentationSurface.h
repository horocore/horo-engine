#pragma once

/** @file EditorPresentationSurface.h
 * @brief Non-installed host coordination for the backend-neutral surface authority.
 */
#include "Horo/Runtime/Render/RenderSurfaceLifecycle.h"

namespace Horo::Editor {
    /** @brief Host-owned frame/cancellation admission at the native safe point. */
    enum class EditorSurfaceAdmission : std::uint8_t {
        Idle,
        FrameActive,
        Cancelled
    };

    /** @brief Explicit owner request to retry a Lost surface after a new platform observation. */
    enum class EditorSurfaceRecovery : std::uint8_t {
        Defer,
        Retry
    };

    /** @brief Surface publication and exact native realization failure, distinct from invalid host admission.
     * A nativeFailure requires Lost with no usable active output. The host keeps pumping events,
     * reports this error, and admits no rendering until an explicit recovery succeeds.
     */
    struct EditorSurfaceObservation final {
        Render::RenderSurfaceSnapshot snapshot;
        std::optional<Error> nativeFailure;
    };

    /** @brief Borrowed owner-thread native resize operation; the host retains its context until Close. */
    class EditorSurfaceRealizer {
    public:
        virtual ~EditorSurfaceRealizer() = default;
        /** @brief Realizes an extent at the host's frozen idle safe point.
         * @param extent Validated nonzero output extent.
         * @return Exact native operation result; no context ownership is transferred.
         */
        [[nodiscard]] virtual Result<void> Resize(Render::FramebufferExtent extent) = 0;
    };

    /** @brief Freezes surface candidates before native work, with no workers, waits or native ownership. */
    class EditorPresentationSurface final {
    public:
        /** @brief Creates one owner-thread authority with a borrowed realizer.
         * @param owner Non-zero host identity.
         * @param realizer Borrowed operation that must outlive this helper.
         * @return Initial unattached authority or a typed admission failure.
         */
        [[nodiscard]] static Result<EditorPresentationSurface> Create(std::uint64_t owner, EditorSurfaceRealizer &realizer);
        EditorPresentationSurface(EditorPresentationSurface &&) noexcept = default;
        EditorPresentationSurface(const EditorPresentationSurface &) = delete;
        EditorPresentationSurface &operator=(const EditorPresentationSurface &) = delete;

        /** @brief Realizes one observed pixel extent at an idle frame boundary.
         * @param extent Absent for minimized/zero-size output; unknown query errors must be returned by the platform adapter.
         * @param mode Present mode already admitted by the selected native backend.
         * @param admission Host frame and cancellation state; only Idle may realize native work.
         * @return Committed snapshot or the exact native error after publishing Lost.
         * @pre No native frame is active. No context restart is performed on recovery.
         */
        [[nodiscard]] Result<Render::RenderSurfaceSnapshot> Update(std::optional<Render::FramebufferExtent> extent,
                                                                   Render::ResolvedPresentMode mode,
                                                                   EditorSurfaceAdmission admission = EditorSurfaceAdmission::Idle);
        /** @brief Applies host observations while retaining native loss as an explicit diagnostic outcome.
         * @param extent Observed pixel extent, absent for minimized/zero-size output.
         * @param mode Already negotiated native presentation policy.
         * @param admission Idle owner-thread admission; cancellation and active frames remain typed errors.
         * @param recovery Retry permits one retained-owner realization attempt; Defer performs no native work while Lost.
         * @return Publication with the exact nativeFailure on loss, or a fatal host/lifecycle admission error.
         * @pre The host reports nativeFailure and does not admit GPU work from a Lost snapshot.
         */
        [[nodiscard]] Result<EditorSurfaceObservation> Observe(std::optional<Render::FramebufferExtent> extent,
                                                               Render::ResolvedPresentMode mode, EditorSurfaceAdmission admission,
                                                               EditorSurfaceRecovery recovery = EditorSurfaceRecovery::Defer);
        /** @brief Stops admission and publishes Close before the host destroys native resources.
         * @return Closed snapshot or typed owner/state failure. Repeated calls are harmless.
         * @param admission Host frame state; cancellation permits Close, an active frame does not.
         * @pre The host has cancelled its active frame; native destruction remains the backend's responsibility.
         */
        [[nodiscard]] Result<Render::RenderSurfaceSnapshot> Close(EditorSurfaceAdmission admission = EditorSurfaceAdmission::Idle);
        /** @brief Returns the sole authority's current publication, or a typed owner-thread error. */
        [[nodiscard]] Result<Render::RenderSurfaceSnapshot> Snapshot() const;

    private:
        /** @brief Transfers the sole lifecycle authority and borrows the host's native operation. */
        EditorPresentationSurface(Render::RenderSurfaceLifecycle surface, EditorSurfaceRealizer &realizer) noexcept;
        /** @brief Admits and freezes one bounded candidate before any native work. */
        [[nodiscard]] Result<Render::RenderSurfaceTransition> Begin(Render::RenderSurfaceCommandKind kind,
                                                                    std::optional<Render::RenderSurfaceConfiguration> target);
        /** @brief Publishes suspension without invoking the native realizer. */
        [[nodiscard]] Result<EditorSurfaceObservation> Suspend(const Render::RenderSurfaceSnapshot &current);
        /** @brief Realizes a frozen candidate and preserves native failure after committing Lost. */
        [[nodiscard]] Result<EditorSurfaceObservation> Realize(Render::FramebufferExtent extent, Render::ResolvedPresentMode mode,
                                                               Render::RenderSurfaceState state);
        /** @brief Rejects host state and unnegotiated mode changes before native work. */
        [[nodiscard]] Result<void> ValidateAdmission(Render::ResolvedPresentMode mode, EditorSurfaceAdmission admission) const;
        Render::RenderSurfaceLifecycle surface_;
        EditorSurfaceRealizer *realizer_; /**< Borrowed typed operation; never deleted by the surface authority. */
        std::optional<Render::ResolvedPresentMode> admittedMode_;
        std::uint64_t sequence_{};
        bool closed_{};
    };
}  // namespace Horo::Editor
