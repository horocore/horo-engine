#pragma once

/**
 * @file OriginRebaseTransaction.h
 * @brief Atomic safe-point publication of one prepared floating-origin replacement.
 */

#include "Horo/WorldStreaming/OriginShiftPolicy.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct OriginRebaseTransactionIdTag;
        struct OriginRebaseParticipantIdTag;
        struct OriginRebaseParticipantRevisionTag;
    }  // namespace Detail

    /** @brief Stable identity of one origin-rebase transaction. */
    using OriginRebaseTransactionId =
        Foundation::Detail::NonZeroId64<Detail::OriginRebaseTransactionIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Stable identity of one subsystem participant. */
    using OriginRebaseParticipantId =
        Foundation::Detail::NonZeroId64<Detail::OriginRebaseParticipantIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact participant implementation/configuration revision. */
    using OriginRebaseParticipantRevision =
        Foundation::Detail::NonZeroId64<Detail::OriginRebaseParticipantRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Host lifecycle governing preparation and publication admission. */
    enum class OriginRebaseLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count,
    };

    /** @brief Host phase supplied when a prepared rebase is published. */
    enum class OriginRebaseCommitPoint : std::uint8_t {
        PreSimulation,
        PostSimulation,
        PostRender,
        Count,
    };

    /** @brief Observable terminal ownership state of one transaction. */
    enum class OriginRebaseTransactionState : std::uint8_t {
        Prepared,
        Published,
        RolledBack,
    };

    /** @brief Stable participant identity and exact readiness-contract revision. */
    struct OriginRebaseParticipantRequirement final {
        OriginRebaseParticipantId participant{};    /**< Stable subsystem participant. */
        OriginRebaseParticipantRevision revision{}; /**< Exact implementation/configuration publication. */

        [[nodiscard]] constexpr auto operator<=>(const OriginRebaseParticipantRequirement &) const noexcept = default;
    };

    /** @brief Immutable old/new frame pair prepared and applied by every participant. */
    struct OriginRebaseEvent final {
        OriginRebaseTransactionId transaction{}; /**< Stable transaction identity. */
        OriginShiftRequestId request{};          /**< Exact authorized shift request. */
        OriginFrame previousFrame;               /**< Active publication captured by policy evaluation. */
        OriginFrame replacementFrame;            /**< Exact successor, invisible until commit. */

        [[nodiscard]] constexpr auto operator<=>(const OriginRebaseEvent &) const noexcept = default;
    };

    /** @brief Exact fences carried by a participant's uniquely owned prepared state. */
    struct OriginRebasePreparedBinding final {
        OriginRebaseParticipantRequirement requirement{}; /**< Participant and exact revision. */
        OriginRebaseTransactionId transaction{};          /**< Transaction for which state was prepared. */
        OriginFrameBinding previousFrame{};               /**< Exact active-frame fence. */
        OriginFrameBinding replacementFrame{};            /**< Exact successor-frame fence. */

        [[nodiscard]] constexpr auto operator<=>(const OriginRebasePreparedBinding &) const noexcept = default;
    };

    /**
     * @brief Unique prepared participant state whose final application cannot fail.
     * @details Implementations keep subsystem/native data private. ApplyPrepared must only publish already-prepared state and must not
     *          allocate, wait, or alter velocity-like state. RollbackPrepared is idempotent and never touches live state.
     */
    class IOriginRebasePreparedParticipant {
    public:
        virtual ~IOriginRebasePreparedParticipant() = default;
        IOriginRebasePreparedParticipant(const IOriginRebasePreparedParticipant &) = delete;
        IOriginRebasePreparedParticipant &operator=(const IOriginRebasePreparedParticipant &) = delete;

        /** @brief Returns all transaction and frame fences. @return Immutable prepared binding. */
        [[nodiscard]] virtual OriginRebasePreparedBinding Binding() const noexcept = 0;
        /** @brief Applies the already-prepared local-frame replacement exactly once without failure. */
        virtual void ApplyPrepared() noexcept = 0;
        /** @brief Releases unpublished prepared state; repeated calls must be harmless. */
        virtual void RollbackPrepared() noexcept = 0;

    protected:
        IOriginRebasePreparedParticipant() = default;
    };

    /** @brief Caller-owned subsystem seam that prepares one fallible rebase receipt. */
    class IOriginRebaseParticipant {
    public:
        virtual ~IOriginRebaseParticipant() = default;
        IOriginRebaseParticipant(const IOriginRebaseParticipant &) = delete;
        IOriginRebaseParticipant &operator=(const IOriginRebaseParticipant &) = delete;

        /** @brief Returns stable participant identity and revision. @return Current immutable requirement. */
        [[nodiscard]] virtual OriginRebaseParticipantRequirement Requirement() const noexcept = 0;
        /**
         * @brief Validates readiness and prepares all fallible state for an event.
         * @param event Exact old/new frame pair.
         * @return Unique prepared state or the participant's typed failure.
         * @post Failure leaves no participant state requiring coordinator rollback.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IOriginRebasePreparedParticipant>> Prepare(
            const OriginRebaseEvent &event) noexcept = 0;

    protected:
        IOriginRebaseParticipant() = default;
    };

    /** @brief Bounded admission facts for one authorized origin-rebase request. */
    struct OriginRebaseTransactionContext final {
        OriginRebaseTransactionId transaction{};                        /**< Unique transaction identity. */
        OriginShiftDecision decision{};                                 /**< Authorized request and exact observed-frame fence. */
        OriginFrame activeFrame;                                        /**< Immutable active publication named by the decision. */
        std::size_t maximumParticipants{};                              /**< Positive hard ceiling for the complete participant set. */
        OriginRebaseLifecycle lifecycle{OriginRebaseLifecycle::Closed}; /**< Current host admission lifecycle. */
    };

    /** @brief Move-only owner of a complete prepared participant set and one successor frame. */
    class OriginRebaseTransaction final {
    public:
        /** @brief Rolls back every prepared participant when publication did not complete. */
        ~OriginRebaseTransaction();
        OriginRebaseTransaction(const OriginRebaseTransaction &) = delete;
        OriginRebaseTransaction &operator=(const OriginRebaseTransaction &) = delete;
        /** @brief Transfers prepared ownership. @param other Source transaction. */
        OriginRebaseTransaction(OriginRebaseTransaction &&other) noexcept;
        /** @brief Rolls back current ownership before transfer. @param other Source transaction. @return This transaction. */
        OriginRebaseTransaction &operator=(OriginRebaseTransaction &&other) noexcept;

        /**
         * @brief Gathers the exact required participants and prepares a complete successor-frame transaction.
         * @param context Authorized shift decision, lifecycle and participant ceiling.
         * @param required Complete canonical set required by host composition.
         * @param participants Caller-owned participant implementations; only prepared receipts are retained.
         * @return Prepared transaction or a typed invalid, unsupported, stale, incomplete, capacity, lifecycle or storage failure.
         * @post Every acquired receipt is rolled back on failure.
         */
        [[nodiscard]] static Result<OriginRebaseTransaction> Prepare(const OriginRebaseTransactionContext &context,
                                                                     std::span<const OriginRebaseParticipantRequirement> required,
                                                                     std::span<IOriginRebaseParticipant *const> participants);

        /**
         * @brief Publishes the successor frame and applies every prepared participant at the host safe point.
         * @param owner Sole owner of the active origin-frame publication.
         * @param commitPoint Current host phase; only PostSimulation is accepted.
         * @param lifecycle Current host lifecycle.
         * @return Success or a typed stale, safe-point, lifecycle or frame-publication failure.
         * @post Success expires old frame leases and applies every participant once in canonical identity order.
         * @post A retryable safe-point failure retains preparation; terminal failure rolls it back and preserves the active frame.
         */
        [[nodiscard]] Result<void> Commit(OriginFrameOwner &owner, OriginRebaseCommitPoint commitPoint, OriginRebaseLifecycle lifecycle);

        /** @brief Explicitly rolls back all prepared receipts in reverse canonical order; idempotent. */
        void Rollback() noexcept;

        /** @brief Returns stable transaction identity. @return Non-zero identity. */
        [[nodiscard]] OriginRebaseTransactionId Id() const noexcept;
        /** @brief Returns immutable old/new frame event. @return Prepared event. */
        [[nodiscard]] const OriginRebaseEvent &Event() const noexcept;
        /** @brief Returns current prepared or terminal state. @return Transaction state. */
        [[nodiscard]] OriginRebaseTransactionState State() const noexcept;
        /** @brief Returns the complete canonical required participant set. @return Borrow valid for this transaction lifetime. */
        [[nodiscard]] std::span<const OriginRebaseParticipantRequirement> Requirements() const noexcept;

    private:
        OriginRebaseTransaction(OriginRebaseTransactionContext context, OriginRebaseEvent event,
                                std::vector<OriginRebaseParticipantRequirement> requirements,
                                std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>> prepared) noexcept;

        OriginRebaseTransactionContext context_;
        OriginRebaseEvent event_;
        std::vector<OriginRebaseParticipantRequirement> requirements_;
        std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>> prepared_;
        OriginRebaseTransactionState state_{OriginRebaseTransactionState::RolledBack};
    };
}  // namespace Horo::WorldStreaming
