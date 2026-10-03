#pragma once

/**
 * @file StreamingSchedulerAdmission.h
 * @brief Atomic scheduler admission and generic capacity reservation for fenced cell operations.
 */

#include "Horo/WorldStreaming/StreamingCellOperation.h"
#include "Horo/WorldStreaming/WorldPartitionCapabilityProfile.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag that keeps scheduler-ledger owners distinct from other non-zero identities. */
        struct StreamingSchedulerLedgerIdTag;
        /** @brief Tag that keeps scheduler reservation identities distinct from operation identities. */
        struct StreamingSchedulerReservationIdTag;
        /** @brief Tag for immutable concurrency policy publications. */
        struct StreamingConcurrencyRevisionTag;
    }  // namespace Detail

    /** @brief Stable identity for one scheduler-ledger owner lifetime; zero is reserved as invalid. */
    using StreamingSchedulerLedgerId =
        Foundation::Detail::NonZeroId64<Detail::StreamingSchedulerLedgerIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Stable reservation identity scoped to one scheduler-ledger owner lifetime. */
    using StreamingSchedulerReservationId =
        Foundation::Detail::NonZeroId64<Detail::StreamingSchedulerReservationIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Non-zero revision of one ledger-owned concurrency policy publication. */
    using StreamingConcurrencyRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingConcurrencyRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /**
     * @brief Complete host-supplied stage ceilings for one exact target profile and revision.
     * @details Stages are operation kinds, not transient execution phases. Interrupted work retains its original
     *          stage slot through retirement; cleanup never needs a second slot or waits for retirement admission.
     *          Zero explicitly disables new work of that kind. No profile implies different limits or a fallback.
     */
    struct StreamingConcurrencyPolicy final {
        WorldPartitionProjectProfile profile{WorldPartitionProjectProfile::Count}; /**< Exact host-selected target profile. */
        StreamingConcurrencyRevision revision;                                     /**< Immutable publication scoped to the ledger owner. */
        std::uint32_t loads{};                                                     /**< Retained Load operations, including rollback. */
        std::uint32_t activations{}; /**< Retained Activate operations, including preparation and rollback. */
        std::uint32_t retirements{}; /**< Retained explicit Retire operations. */

        /** @brief Checks revision and bounded stage ceilings. @return True for a known profile and at least one enabled stage. */
        [[nodiscard]] bool IsValid() const noexcept;
        /**
         * @brief Looks up the exact stage ceiling without changing policy.
         * @param kind Requested operation kind.
         * @return Ceiling (zero means disabled), or typed unsupported-kind failure.
         */
        [[nodiscard]] Result<std::uint32_t> Limit(StreamingCellOperationKind kind) const;
        [[nodiscard]] constexpr auto operator<=>(const StreamingConcurrencyPolicy &) const noexcept = default;
    };

    /** @brief Admission lifecycle owned by the partition authority's scheduler ledger. */
    enum class StreamingSchedulerAdmissionState : std::uint8_t {
        Accepting,
        Draining,
        Closed,
    };

    /** @brief Bounded total and stage/profile limits independent of multidimensional byte/time policy. */
    struct StreamingSchedulerAdmissionLimits final {
        /** @brief Mandatory implementation ceiling that bounds preallocated ledger storage. */
        static constexpr std::uint32_t MaximumConcurrentOperations = 1024;

        std::uint32_t concurrentOperations{};   /**< Maximum simultaneously retained operation reservations. */
        std::uint64_t capacityUnits{};          /**< Generic capacity supplied by the owning host policy. */
        StreamingConcurrencyPolicy concurrency; /**< Mandatory profile/revision and independent stage ceilings. */

        /** @brief Validates complete bounded limits. @return True for positive total ceilings and a valid stage/profile policy. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const StreamingSchedulerAdmissionLimits &) const noexcept = default;
    };

    /** @brief Exact generic-capacity reservation retained until its canonical operation becomes terminal. */
    struct StreamingSchedulerReservation final {
        StreamingSchedulerLedgerId owner;       /**< Exact ledger-owner lifetime. */
        StreamingSchedulerReservationId id;     /**< Monotonic identity within the owner lifetime. */
        StreamingCellOperationHandle operation; /**< Operation and fence that own the reservation. */
        std::uint64_t capacityUnits{};          /**< Generic capacity charged by this admission. */

        /** @brief Checks the complete reservation representation. @return True when all identities and capacity are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const StreamingSchedulerReservation &) const noexcept = default;
    };

    /**
     * @brief Bounded admission ledger that owns canonical admitted operation state.
     * @details Mutating calls are confined to the host's StreamingAuthorityRole; this value intentionally adds no mutex.
     *          The owning authority must drain or transfer every retained reservation before destroying the ledger.
     */
    class StreamingSchedulerAdmissionLedger final {
    public:
        StreamingSchedulerAdmissionLedger(const StreamingSchedulerAdmissionLedger &) = delete;
        StreamingSchedulerAdmissionLedger &operator=(const StreamingSchedulerAdmissionLedger &) = delete;
        /** @brief Transfers all retained ownership and closes the source admission gate. @param other Unique source owner. */
        StreamingSchedulerAdmissionLedger(StreamingSchedulerAdmissionLedger &&other) noexcept;
        StreamingSchedulerAdmissionLedger &operator=(StreamingSchedulerAdmissionLedger &&) = delete;

        /**
         * @brief Creates an accepting ledger with bounded preallocated reservation storage.
         * @param owner Unique non-zero identity for this ledger-owner lifetime.
         * @param limits Positive total ceilings and a complete explicit stage/profile policy.
         * @return Empty ledger or a typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<StreamingSchedulerAdmissionLedger> Create(StreamingSchedulerLedgerId owner,
                                                                              const StreamingSchedulerAdmissionLimits &limits);

        /**
         * @brief Atomically reserves required capacity and admits one exact queued operation.
         * @param operation Queued operation whose canonical admitted successor becomes ledger-owned.
         * @param requiredCapacityUnits Positive generic capacity required before work can start.
         * @param expectedRevision Exact current concurrency policy revision captured by the submitting owner.
         * @return Exact reservation, or a typed failure without ledger or operation mutation.
         * @pre Called on the owning StreamingAuthorityRole.
         */
        [[nodiscard]] Result<StreamingSchedulerReservation> TryAdmit(const StreamingCellOperation &operation,
                                                                     std::uint64_t requiredCapacityUnits,
                                                                     StreamingConcurrencyRevision expectedRevision);

        /**
         * @brief Replaces the complete stage policy without altering retained operations or reservations.
         * @param expectedRevision Exact current policy revision.
         * @param policy Same target profile with a strictly newer revision and complete bounded ceilings.
         * @return Success or typed invalid, unsupported, stale, or lifecycle failure without mutation.
         * @details Lowered ceilings may be below retained counts. New work then waits for real releases;
         *          existing exact reservation tokens remain routable under their original ownership.
         * @pre Called on StreamingAuthorityRole while accepting.
         */
        [[nodiscard]] Result<void> ReplaceConcurrency(StreamingConcurrencyRevision expectedRevision,
                                                      const StreamingConcurrencyPolicy &policy);

        /**
         * @brief Counts retained operations of one kind, including terminal work awaiting explicit release.
         * @param kind Exact operation kind.
         * @return Count or typed unsupported-kind failure; performs no allocation.
         */
        [[nodiscard]] Result<std::size_t> ReservedCount(StreamingCellOperationKind kind) const;

        /**
         * @brief Advances the ledger-owned canonical operation for an exact reservation.
         * @param reservation Exact token returned by TryAdmit.
         * @param transition Typed lifecycle transition to apply.
         * @return Canonical successor snapshot, or a typed failure without ledger mutation.
         * @pre Called on the owning StreamingAuthorityRole; worker completions must first route to that role.
         */
        [[nodiscard]] Result<StreamingCellOperation> Advance(const StreamingSchedulerReservation &reservation,
                                                             StreamingCellOperationTransition transition);

        /**
         * @brief Releases exact capacity only after the ledger-owned operation is terminal.
         * @param reservation Exact token returned by TryAdmit.
         * @return Success, or a typed failure without ledger mutation.
         * @pre Called on the owning StreamingAuthorityRole.
         */
        [[nodiscard]] Result<void> Release(const StreamingSchedulerReservation &reservation);

        /** @brief Stops new admission while retained reservations drain. @pre Called on the owning StreamingAuthorityRole. */
        void BeginShutdown() noexcept;

        /** @brief Returns the exact ledger-owner identity. @return Non-zero owner lifetime. */
        [[nodiscard]] StreamingSchedulerLedgerId Owner() const noexcept;
        /** @brief Returns the total ceilings and current concurrency publication. @return Ledger limits. */
        [[nodiscard]] StreamingSchedulerAdmissionLimits Limits() const noexcept;
        /** @brief Returns the admission lifecycle. @return Accepting, Draining, or Closed. */
        [[nodiscard]] StreamingSchedulerAdmissionState State() const noexcept;
        /** @brief Returns operations still owned by this ledger. @return Current reservation count. */
        [[nodiscard]] std::size_t ReservedCount() const noexcept;
        /** @brief Returns generic capacity still charged by retained operations. @return Current capacity charge. */
        [[nodiscard]] std::uint64_t ReservedCapacityUnits() const noexcept;

    private:
        struct Entry final {
            StreamingSchedulerReservation reservation;
            StreamingCellOperation operation;
        };

        StreamingSchedulerAdmissionLedger(StreamingSchedulerLedgerId owner, const StreamingSchedulerAdmissionLimits &limits) noexcept;

        StreamingSchedulerLedgerId owner_{};
        StreamingSchedulerAdmissionLimits limits_{};
        StreamingSchedulerAdmissionState state_{StreamingSchedulerAdmissionState::Accepting};
        std::uint64_t reservedCapacityUnits_{};
        std::uint64_t nextReservationValue_{1};
        std::vector<Entry> entries_;
    };
}  // namespace Horo::WorldStreaming
