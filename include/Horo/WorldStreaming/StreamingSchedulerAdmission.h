#pragma once

/**
 * @file StreamingSchedulerAdmission.h
 * @brief Atomic scheduler admission and generic capacity reservation for fenced cell operations.
 */

#include "Horo/WorldStreaming/StreamingCellOperation.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag that keeps scheduler-ledger owners distinct from other non-zero identities. */
        struct StreamingSchedulerLedgerIdTag;
        /** @brief Tag that keeps scheduler reservation identities distinct from operation identities. */
        struct StreamingSchedulerReservationIdTag;
    }  // namespace Detail

    /** @brief Stable identity for one scheduler-ledger owner lifetime; zero is reserved as invalid. */
    using StreamingSchedulerLedgerId =
        Foundation::Detail::NonZeroId64<Detail::StreamingSchedulerLedgerIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Stable reservation identity scoped to one scheduler-ledger owner lifetime. */
    using StreamingSchedulerReservationId =
        Foundation::Detail::NonZeroId64<Detail::StreamingSchedulerReservationIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Admission lifecycle owned by the partition authority's scheduler ledger. */
    enum class StreamingSchedulerAdmissionState : std::uint8_t {
        Accepting,
        Draining,
        Closed,
    };

    /** @brief Bounded scheduler limits independent of the later multidimensional budget policy. */
    struct StreamingSchedulerAdmissionLimits final {
        /** @brief Mandatory implementation ceiling that bounds preallocated ledger storage. */
        static constexpr std::uint32_t MaximumConcurrentOperations = 1024;

        std::uint32_t concurrentOperations{}; /**< Maximum simultaneously retained operation reservations. */
        std::uint64_t capacityUnits{};        /**< Generic capacity supplied by the owning host policy. */

        /** @brief Validates positive bounded limits. @return True when both ceilings can be represented safely. */
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
        StreamingSchedulerAdmissionLedger(StreamingSchedulerAdmissionLedger &&) noexcept = default;
        StreamingSchedulerAdmissionLedger &operator=(StreamingSchedulerAdmissionLedger &&) noexcept = default;

        /**
         * @brief Creates an accepting ledger with bounded preallocated reservation storage.
         * @param owner Unique non-zero identity for this ledger-owner lifetime.
         * @param limits Positive concurrent-operation and generic-capacity ceilings.
         * @return Empty ledger or a typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<StreamingSchedulerAdmissionLedger> Create(StreamingSchedulerLedgerId owner,
                                                                              StreamingSchedulerAdmissionLimits limits);

        /**
         * @brief Atomically reserves required capacity and admits one exact queued operation.
         * @param operation Queued operation whose canonical admitted successor becomes ledger-owned.
         * @param requiredCapacityUnits Positive generic capacity required before work can start.
         * @return Exact reservation, or a typed failure without ledger or operation mutation.
         * @pre Called on the owning StreamingAuthorityRole.
         */
        [[nodiscard]] Result<StreamingSchedulerReservation> TryAdmit(const StreamingCellOperation &operation,
                                                                     std::uint64_t requiredCapacityUnits);

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

        /** @brief Inspects the ledger-owned operation without permitting caller mutation.
         * @param reservation Exact retained reservation. @return Canonical snapshot or typed invalid/stale failure. */
        [[nodiscard]] Result<StreamingCellOperation> Inspect(const StreamingSchedulerReservation &reservation) const;

        /** @brief Stops new admission while retained reservations drain. @pre Called on the owning StreamingAuthorityRole. */
        void BeginShutdown() noexcept;

        /** @brief Returns the exact ledger-owner identity. @return Non-zero owner lifetime. */
        [[nodiscard]] StreamingSchedulerLedgerId Owner() const noexcept;
        /** @brief Returns the configured immutable ceilings. @return Ledger limits. */
        [[nodiscard]] StreamingSchedulerAdmissionLimits Limits() const noexcept;
        /** @brief Returns the admission lifecycle. @return Accepting, Draining, or Closed. */
        [[nodiscard]] StreamingSchedulerAdmissionState State() const noexcept;
        /** @brief Returns operations still owned by this ledger. @return Current reservation count. */
        [[nodiscard]] std::size_t ReservedCount() const noexcept;
        /** @brief Returns generic capacity still charged by retained operations. @return Current capacity charge. */
        [[nodiscard]] std::uint64_t ReservedCapacityUnits() const noexcept;

    private:
        [[nodiscard]] Result<std::size_t> FindExact(const StreamingSchedulerReservation &reservation) const;

        struct Entry final {
            StreamingSchedulerReservation reservation;
            StreamingCellOperation operation;
        };

        StreamingSchedulerAdmissionLedger(StreamingSchedulerLedgerId owner, StreamingSchedulerAdmissionLimits limits) noexcept;

        StreamingSchedulerLedgerId owner_{};
        StreamingSchedulerAdmissionLimits limits_{};
        StreamingSchedulerAdmissionState state_{StreamingSchedulerAdmissionState::Accepting};
        std::uint64_t reservedCapacityUnits_{};
        std::uint64_t nextReservationValue_{1};
        std::vector<Entry> entries_;
    };
}  // namespace Horo::WorldStreaming
