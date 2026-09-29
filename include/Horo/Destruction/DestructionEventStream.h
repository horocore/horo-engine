#pragma once

/**
 * @file DestructionEventStream.h
 * @brief Bounded, owner-thread committed destruction facts and generation-fenced cursors.
 */

#include "Horo/Destruction/DestructibleDescriptor.h"
#include "Horo/Destruction/DestructionIdentity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Destruction {
    /** @brief Fixed schema version for the baseline semantic fact payload. */
    inline constexpr std::uint16_t DestructionFactSchema = 1;

    /** @brief Finite semantic evidence; absent fields remain invalid/zero, never inferred by consumers. */
    struct DestructionFactPayload final {
        DestructionChunkId chunk{};    /**< Optional committed chunk identity. */
        std::array<float, 3> point{};  /**< Optional finite world-space point. */
        std::array<float, 3> normal{}; /**< Optional finite world-space normal. */
        float strength{};              /**< Finite, non-negative semantic magnitude. */
        bool hasPoint{};               /**< Whether point is present. */
        bool hasNormal{};              /**< Whether normal is present. */
    };

    /** @brief One immutable post-aggregate-commit semantic fact, with no consumer asset or native handle. */
    struct DestructionFact final {
        DestructionEventOccurrenceId occurrence{};          /**< Exact source/revision/kind/ordinal. */
        std::uint64_t transitionTicket{};                   /**< Non-zero owner-issued aggregate transition identity. */
        std::uint64_t committedTick{};                      /**< Non-zero fixed-tick publication order. */
        std::uint16_t payloadSchema{DestructionFactSchema}; /**< Exact payload schema. */
        DestructionFactPayload payload{};                   /**< Copied finite semantic evidence. */
    };

    /** @brief Cursor into one world incarnation; sequence zero is the initial position. */
    struct DestructionEventCursor final {
        DestructionWorldId world{}; /**< Exact source world generation. */
        std::uint64_t sequence{};   /**< Number of facts preceding the next read. */
        [[nodiscard]] auto operator<=>(const DestructionEventCursor &) const noexcept = default;
    };

    /** @brief Typed journal or dispatcher outcome; never a boolean capability fallback. */
    enum class DestructionEventStatus : std::uint8_t {
        Ok,
        Empty,
        Invalid,
        WrongThread,
        StaleGeneration,
        StaleRevision,
        CapacityExceeded,
        Gap,
        ReservationStale,
        Cancelled,
        ShutdownInProgress,
        RequiredConsumerStalled,
        ConsumerUnavailable,
        ConsumerCapacityExceeded,
        AlreadyDispatched,
        OccurrenceExpired,
    };

    /** @brief Move-only prevalidated fact batch; dropping it cancels publication without changing the journal. */
    class DestructionEventReservation final {
    public:
        DestructionEventReservation() = default;
        DestructionEventReservation(DestructionEventReservation &&) noexcept = default;
        DestructionEventReservation &operator=(DestructionEventReservation &&) noexcept = default;
        DestructionEventReservation(const DestructionEventReservation &) = delete;
        DestructionEventReservation &operator=(const DestructionEventReservation &) = delete;

    private:
        friend class DestructionEventStream;
        DestructionHandle source_{};
        std::uint64_t nextSequence_{};
        std::uint32_t count_{};
        std::vector<DestructionFact> facts_{};
    };

    /** @brief Result of one bounded cursor read, including a recovery cursor on Gap. */
    struct DestructionEventRead final {
        DestructionEventStatus status{DestructionEventStatus::Invalid}; /**< Explicit read outcome. */
        DestructionEventCursor next{};                                  /**< Next cursor or oldest retained cursor on Gap. */
        DestructionFact fact{};                                         /**< Valid only when status is Ok. */
    };

    /**
     * @brief Scene-scoped, single-owner bounded fact journal; not a save, replication or effect store.
     * @details Construct at scene activation. Reserve before the Scene/Physics/Render aggregate commit, then call Publish
     * only after that aggregate is visible. No consumer callback occurs in Publish. Readers copy facts at owner safe points.
     * The owner must reconcile a Gap against a canonical scene snapshot before installing its returned cursor.
     */
    class DestructionEventStream final {
    public:
        /**
         * @brief Allocates finite journal storage at activation, never during publication.
         * @param world Exact scene-scoped world incarnation.
         * @param capacity Retained fact bound, 1..DestructionHardLimits::EventJournalEntries.
         * @param maximumBatch Per-transition bound, 1..min(capacity, EventsPerTransition).
         * @return Stream or explicit invalid/capacity status.
         */
        [[nodiscard]] static std::pair<DestructionEventStatus, std::unique_ptr<DestructionEventStream>> Create(DestructionWorldId world,
                                                                                                               std::uint32_t capacity,
                                                                                                               std::uint32_t maximumBatch);

        /** @brief Returns the cursor after the last committed fact. @return World-fenced tail cursor. */
        [[nodiscard]] DestructionEventCursor Tail() const noexcept;
        /** @brief Returns the oldest retained cursor. @return World-fenced recovery cursor. */
        [[nodiscard]] DestructionEventCursor Oldest() const noexcept;
        /** @brief Returns whether this thread owns the stream. @return True only on its construction thread. */
        [[nodiscard]] bool OnOwnerThread() const noexcept;

        /**
         * @brief Validates and owns the exact bounded batch before an aggregate transition is committed.
         * @param source Exact current canonical source handle, supplied by the aggregate owner.
         * @param sourceRevision Canonical pre-commit revision for that source and generation, supplied by the aggregate owner.
         * @param transitionTicket Non-zero unique owner-issued ticket.
         * @param facts Complete canonically ordered planned batch, copied before commit.
         * @param requiredCursor Minimum of all required consumer acknowledgements; optional consumers do not hold retention.
         * @return Reservation or typed failure. No journal mutation occurs.
         */
        [[nodiscard]] std::pair<DestructionEventStatus, DestructionEventReservation> Reserve(
            DestructionHandle source, DestructionStateRevision sourceRevision, std::uint64_t transitionTicket,
            std::span<const DestructionFact> facts, DestructionEventCursor requiredCursor) const noexcept;

        /**
         * @brief Atomically appends the exact planned batch after the complete aggregate root is visible.
         * @param reservation Move-only prevalidated batch; consumed on success.
         * @param currentSource Exact canonical source handle after aggregate commit; rejects replacement.
         * @param committedRevision Canonical post-commit revision; must equal the reserved fact revision.
         * @return Typed result; failure leaves the journal unchanged.
         * @pre Caller owns the scene safe point and has committed the corresponding Scene/Physics/Render aggregate.
         */
        [[nodiscard]] DestructionEventStatus Publish(DestructionEventReservation &&reservation, DestructionHandle currentSource,
                                                     DestructionStateRevision committedRevision) noexcept;

        /**
         * @brief Copies one committed fact or reports a gap requiring full canonical snapshot reconciliation.
         * @param cursor World-fenced position to read.
         * @return Fact and next cursor, Empty, Gap, or typed lifecycle failure.
         */
        [[nodiscard]] DestructionEventRead Read(DestructionEventCursor cursor) const noexcept;

        /** @brief Fences admission while preserving the final readable journal prefix. */
        void BeginShutdown() noexcept;

    private:
        DestructionEventStream(DestructionWorldId world, std::uint32_t capacity, std::uint32_t maximumBatch,
                               std::vector<DestructionFact> storage) noexcept;
        [[nodiscard]] DestructionEventStatus ValidateBatch(DestructionHandle source, DestructionStateRevision sourceRevision,
                                                           std::uint64_t transitionTicket,
                                                           std::span<const DestructionFact> facts) const noexcept;

        DestructionWorldId world_{};
        std::uint32_t capacity_{};
        std::uint32_t maximumBatch_{};
        std::vector<DestructionFact> storage_;
        std::thread::id owner_;
        std::uint64_t tail_{};
        std::uint64_t oldest_{};
        std::uint64_t lastTick_{};
        bool closing_{};
    };
}  // namespace Horo::Destruction
