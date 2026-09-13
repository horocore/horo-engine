#pragma once

/**
 * @file RenderSurfaceLifecycle.h
 * @brief Backend-neutral primary-surface generation and safe-point transition contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/PresentMode.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <cstdint>
#include <optional>
#include <thread>

namespace Horo::Render {
    /** @brief Machine-local owner and renderer-surface generation identity. */
    struct RenderSurfaceId final {
        std::uint64_t owner{};      /**< Non-zero host-assigned owner identity. */
        std::uint64_t generation{}; /**< Zero before first attachment; otherwise monotonically increasing. */

        /** @brief Reports whether the owner identity is usable. */
        [[nodiscard]] constexpr bool HasOwner() const noexcept {
            return owner != 0;
        }

        /** @brief Reports whether this value identifies an attached generation. */
        [[nodiscard]] constexpr bool IsAttachedGeneration() const noexcept {
            return HasOwner() && generation != 0;
        }

        [[nodiscard]] constexpr bool operator==(const RenderSurfaceId &) const noexcept = default;
    };

    /** @brief Admitted native-free configuration for one ready surface generation. */
    struct RenderSurfaceConfiguration final {
        FramebufferExtent extent;        /**< Non-zero drawable pixel extent. */
        ResolvedPresentMode presentMode; /**< Explicit negotiated present-mode result. */
        std::uint64_t displayRevision{}; /**< Non-zero platform display/window fact revision. */

        [[nodiscard]] bool operator==(const RenderSurfaceConfiguration &) const noexcept = default;
    };

    /** @brief Published renderer-owned primary-surface lifecycle state. */
    enum class RenderSurfaceState : std::uint8_t {
        Unattached,
        Ready,
        Suspended,
        Reconfiguring,
        Lost,
        Closing,
    };

    /** @brief Bounded host or platform request kind accepted by the surface owner. */
    enum class RenderSurfaceCommandKind : std::uint8_t {
        Attach,
        Resize,
        Suspend,
        Replace,
        Lose,
        Recover,
        Close,
    };

    /** @brief Revisioned request queued for the next renderer safe point. */
    struct RenderSurfaceCommand final {
        std::uint64_t sequence{};                         /**< Non-zero monotonically increasing owner sequence. */
        RenderSurfaceCommandKind kind{};                  /**< Exact requested lifecycle operation. */
        std::optional<RenderSurfaceConfiguration> target; /**< Required new configuration only for realizing operations. */

        [[nodiscard]] bool operator==(const RenderSurfaceCommand &) const noexcept = default;
    };

    /** @brief Immutable logical publication observed by host and renderer consumers. */
    struct RenderSurfaceSnapshot final {
        RenderSurfaceId surface;                                  /**< Owner and currently published generation. */
        std::uint64_t revision{};                                 /**< Non-zero lifecycle publication revision. */
        RenderSurfaceState state{RenderSurfaceState::Unattached}; /**< Explicit acquisition-admission state. */
        std::optional<RenderSurfaceConfiguration> active;         /**< Last successfully realized output, when retained. */
        std::optional<std::uint64_t> candidateSequence;           /**< Frozen transition currently awaiting realization. */

        [[nodiscard]] bool operator==(const RenderSurfaceSnapshot &) const noexcept = default;
    };

    /** @brief Whether queue admission used an empty slot or replaced a not-yet-frozen request. */
    enum class RenderSurfaceQueueDisposition : std::uint8_t {
        Accepted,
        Coalesced,
    };

    /** @brief Observable bounded-queue result, including the explicitly superseded request. */
    struct RenderSurfaceQueueResult final {
        RenderSurfaceQueueDisposition disposition{RenderSurfaceQueueDisposition::Accepted}; /**< Admission outcome. */
        std::optional<std::uint64_t> supersededSequence; /**< Replaced pending request, when coalesced. */

        [[nodiscard]] bool operator==(const RenderSurfaceQueueResult &) const noexcept = default;
    };

    /** @brief Frozen exact transition passed to private native realization at RenderExecution entry. */
    struct RenderSurfaceTransition final {
        RenderSurfaceId sourceSurface;  /**< Exact source owner and generation. */
        std::uint64_t sourceRevision{}; /**< Exact publication from which work began. */
        RenderSurfaceCommand command;   /**< Frozen request; later events cannot mutate it. */

        [[nodiscard]] bool operator==(const RenderSurfaceTransition &) const noexcept = default;
    };

    /** @brief Native realization disposition used to publish or roll back a frozen transition. */
    enum class RenderSurfaceRealization : std::uint8_t {
        Ready,
        Suspended,
        Lost,
        PreserveActive,
        Unattached,
    };

    /**
     * @brief Owner-thread bounded state machine for one current primary native surface.
     *
     * Queue retains at most one not-yet-frozen request and coalesces later requests with
     * explicit supersession evidence. BeginFrameBoundary freezes exactly one candidate;
     * Complete publishes native success or an honest rollback/suspended/lost outcome.
     * The class owns no native handles and performs no waits.
     */
    class RenderSurfaceLifecycle final {
    public:
        /**
         * @brief Creates an unattached lifecycle owned by the calling thread.
         * @param owner Non-zero machine-local host owner identity.
         * @return Lifecycle or a typed invalid-identity failure.
         */
        [[nodiscard]] static Result<RenderSurfaceLifecycle> Create(std::uint64_t owner);

        RenderSurfaceLifecycle(const RenderSurfaceLifecycle &) = delete;
        RenderSurfaceLifecycle &operator=(const RenderSurfaceLifecycle &) = delete;
        /**
         * @brief Transfers the complete lifecycle authority and leaves the source inert.
         * @param other Lifecycle whose exact pending and in-flight state is transferred.
         */
        RenderSurfaceLifecycle(RenderSurfaceLifecycle &&other) noexcept;
        RenderSurfaceLifecycle &operator=(RenderSurfaceLifecycle &&) = delete;

        /**
         * @brief Queues or coalesces one newer request without changing a frozen transition.
         * @param command Complete typed request from the platform/host owner.
         * @return Admission disposition and superseded sequence, or a typed validation/state failure.
         */
        [[nodiscard]] Result<RenderSurfaceQueueResult> Queue(RenderSurfaceCommand command);

        /**
         * @brief Freezes the pending request at renderer frame-boundary entry.
         * @return Exact candidate for native realization, or typed no-pending/busy/thread failure.
         * @post No backend frame may already be active when the host invokes this method.
         */
        [[nodiscard]] Result<RenderSurfaceTransition> BeginFrameBoundary();

        /**
         * @brief Commits an exact frozen candidate after private native realization.
         * @param transition Candidate returned by BeginFrameBoundary.
         * @param realization Honest native result; invalid command/outcome pairs are rejected.
         * @return Newly published snapshot, or a typed stale/outcome/thread failure.
         */
        [[nodiscard]] Result<RenderSurfaceSnapshot> Complete(const RenderSurfaceTransition &transition,
                                                             RenderSurfaceRealization realization);

        /**
         * @brief Returns the current immutable publication on the owner thread.
         * @return Current snapshot or a typed thread-affinity failure.
         */
        [[nodiscard]] Result<RenderSurfaceSnapshot> Snapshot() const;

        /**
         * @brief Reports whether one frozen native transition is outstanding.
         * @return True when Complete must resolve a frozen transition, or a typed thread-affinity failure.
         */
        [[nodiscard]] Result<bool> HasTransitionInFlight() const;

        /**
         * @brief Reports whether one request is retained for a future safe point.
         * @return True when BeginFrameBoundary can freeze pending work, or a typed thread-affinity failure.
         */
        [[nodiscard]] Result<bool> HasPendingRequest() const;

    private:
        struct ConstructionKey {};

        RenderSurfaceLifecycle(std::uint64_t owner, ConstructionKey) noexcept;

        std::thread::id ownerThread_;
        RenderSurfaceSnapshot snapshot_;
        std::optional<RenderSurfaceCommand> pending_;
        std::optional<RenderSurfaceTransition> inFlight_;
        RenderSurfaceState stateBeforeTransition_{RenderSurfaceState::Unattached};
        std::optional<RenderSurfaceConfiguration> activeBeforeTransition_;
        std::uint64_t lastAcceptedSequence_{};
    };
}  // namespace Horo::Render
