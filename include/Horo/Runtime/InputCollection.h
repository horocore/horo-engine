#pragma once

/** @file InputCollection.h
 * @brief Owner-thread, bounded semantic-source collection into the canonical InputRouter commit.
 */
#include "Horo/Runtime/Input.h"

namespace Horo::Input {
    inline constexpr std::size_t MaximumCollectedInputSources = 4;
    inline constexpr std::size_t MaximumCollectedInputActions = 256;
    struct InputCollectionLifetime;

    /** @brief One scalar candidate keyed by its position in a validated source registration, never a new action ID. */
    struct CollectedActionValue final {
        std::uint16_t actionIndex{}; /**< Exact registration-local action index. */
        float x{};                   /**< Normalized finite component. */
        float y{};                   /**< Second component for Axis2D only. */
        bool active{};               /**< Inactive candidates must have exact zero components. */
    };

    /** @brief Move-only registration tied to the router, action map, context and host-selected assignment incarnation. */
    class InputActionSourceToken final {
    public:
        InputActionSourceToken() = default;
        ~InputActionSourceToken();
        InputActionSourceToken(InputActionSourceToken &&other) noexcept;
        InputActionSourceToken &operator=(InputActionSourceToken &&other) noexcept;
        InputActionSourceToken(const InputActionSourceToken &) = delete;
        InputActionSourceToken &operator=(const InputActionSourceToken &) = delete;
        /** @brief Revokes this source on the owner thread. @return Typed stale/owner-thread failure, or success.
         * @post Replacement requires a subsequent actual neutral commit. Destruction off-thread never touches the router;
         * its bounded registration remains reserved until owner retirement. */
        [[nodiscard]] Result<void> Reset();
        /** @brief Revalidates this exact reserved source without publishing or changing its registration.
         * @return Success or the original typed retired-router, wrong-thread, stale map/context/assignment/source error.
         * @post Owner-thread success performs no allocation, native callback, input commit or mutation.
         * @details Used by host preparation to bind an already reserved source before publishing another subsystem Ready.
         * The weak lifetime fence is checked before touching the router; moved/reset tokens are stale.
         */
        [[nodiscard]] Result<void> Validate() const;

    private:
        friend class InputActionCollection;
        friend class InputCollectionFrame;
        void Abandon() noexcept;
        std::weak_ptr<InputCollectionLifetime> owner_;
        std::uint64_t generation_{};
        std::size_t slot_{};
    };

    /** @brief One unpublished input cutoff; destruction cancels rather than publishing a partial candidate. */
    class InputCollectionFrame final {
    public:
        InputCollectionFrame() = default;
        ~InputCollectionFrame();
        InputCollectionFrame(InputCollectionFrame &&other) noexcept;
        InputCollectionFrame &operator=(InputCollectionFrame &&other) noexcept;
        InputCollectionFrame(const InputCollectionFrame &) = delete;
        InputCollectionFrame &operator=(const InputCollectionFrame &) = delete;
        /** @brief Atomically stages one source's values. @param source Exact current registration.
         * @param values Unique registration-local indices and normalized payloads.
         * @return Typed stale, invalid, capacity or owner-thread error without partial admission. */
        [[nodiscard]] Result<void> Append(const InputActionSourceToken &source, std::span<const CollectedActionValue> values) const;
        /** @brief Publishes the raw snapshot and all staged values once through the existing router ledger.
         * @return Success or typed stale/owner-thread failure. Missing sources commit neutral state.
         * @pre The raw snapshot supplied to Begin remains alive through this frame's routing, as for BeginFrame. */
        [[nodiscard]] Result<void> Commit();
        /** @brief Discards this exact unpublished cutoff. @return Success or typed stale/owner-thread failure.
         * @post Routing stays unavailable until an actual later commit; no partial values escape. */
        [[nodiscard]] Result<void> Cancel();

    private:
        friend class InputActionCollection;
        void Abandon() noexcept;
        std::weak_ptr<InputCollectionLifetime> owner_;
        std::uint64_t serial_{};
    };

    /** @brief Borrowed, assignment-bound producer port; it cannot create actions, assign players or consume transitions.
     * All methods run on the creating router thread. Router destruction safely invalidates every borrow/token.
     * InputRouter is nonmovable; map/profile/assignment changes invalidate registrations. */
    class InputActionCollection final {
    public:
        /** @brief Registers existing canonical descriptors in this exact context. @param actions Exact registered schemas.
         * @return Move-only source, or typed invalid/stale/capacity denial. Registration may allocate only in Result errors.
         * @post At most four sources and 256 actions total; replacement waits for a real neutral commit. */
        [[nodiscard]] Result<InputActionSourceToken> RegisterSource(std::span<const ActionDescriptor> actions) const;
        /** @brief Opens one collection cutoff. @param raw Immutable raw snapshot with the normal routing lifetime.
         * @return Move-only transaction or typed stale/owner-thread/sequence denial. */
        [[nodiscard]] Result<InputCollectionFrame> Begin(const RawInputSnapshot &raw) const;

    private:
        friend class InputRouter;
        std::weak_ptr<InputCollectionLifetime> owner_;
        std::uint64_t context_{};
        std::uint64_t configuration_{};
        std::uint64_t assignment_{};
        std::optional<PlayerId> player_;
        std::optional<GamepadDeviceId> device_;
    };
}  // namespace Horo::Input
