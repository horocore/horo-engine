#pragma once

/**
 * @file StreamingOwnerFrameBudget.h
 * @brief Shared owner-frame admission for bounded publication and retirement units.
 */

#include "Horo/WorldStreaming/StreamingSchedulerAdmission.h"

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Separates owner-work policy publications from concurrency policies. */
        struct StreamingOwnerWorkRevisionTag;
        /** @brief Separates monotonically issued frame identities from other owner counters. */
        struct StreamingOwnerFrameIdTag;
    }  // namespace Detail

    /** @brief Immutable host publication of owner-work limits. */
    using StreamingOwnerWorkRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingOwnerWorkRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Monotonic frame identity scoped to one scheduler lifetime. */
    using StreamingOwnerFrameId = Foundation::Detail::NonZeroId64<Detail::StreamingOwnerFrameIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Complete immutable frame policy supplied once by the scheduler owner. */
    struct StreamingOwnerFrameLimits final {
        StreamingSchedulerLedgerId owner;          /**< Scheduler lifetime shared by current and retiring attempts. */
        StreamingOwnerWorkRevision policyRevision; /**< Non-zero host policy publication; never reused for changed limits. */
        StreamingOwnerFrameId frame;               /**< Non-zero monotonic host frame; never reset during an owner lifetime. */
        std::uint64_t maximumNanoseconds{};        /**< Positive owner-work target, independent of resource byte limits. */
        std::uint32_t maximumUnits{};              /**< Positive hard ceiling on work units started in this frame. */
    };

    /**
     * @brief Unique allocation-free ledger shared by every cell's work in one owner frame.
     * @details Confined to StreamingAuthorityRole. The host creates exactly one ledger per frame and shares it across
     *          activation, rollback/retirement and old/new partition attempts; recreating it mid-frame bypasses the contract.
     *          Conservative unit charges and elapsed-time samples both stop new work. Native calls are never preempted.
     *          No resource reservation is released by this ledger. Its lifetime ends after the owner frame ends.
     */
    class StreamingOwnerFrameBudget final {
    public:
        StreamingOwnerFrameBudget(const StreamingOwnerFrameBudget &) = delete;
        StreamingOwnerFrameBudget &operator=(const StreamingOwnerFrameBudget &) = delete;
        /** @brief Transfers frame accounting and closes the source. @param other Unique source budget. */
        StreamingOwnerFrameBudget(StreamingOwnerFrameBudget &&other) noexcept;
        StreamingOwnerFrameBudget &operator=(StreamingOwnerFrameBudget &&) = delete;

        /** @brief Validates positive complete frame facts. @param limits Exact owner/frame policy. @return Budget or typed invalid failure.
         */
        [[nodiscard]] static Result<StreamingOwnerFrameBudget> Create(const StreamingOwnerFrameLimits &limits);
        /**
         * @brief Charges a complete bounded unit before its callbacks run.
         * @param owner Exact scheduler lifetime requesting work.
         * @param maximumNanoseconds Validated positive upper bound for the entire unit, including adapter destruction.
         * @param elapsedNanoseconds Host monotonic elapsed time since this frame's owner-work service began, sampled immediately before
         * admission.
         * @return True when charged, false for normal frame deferral, or typed invalid, stale or oversized-unit failure.
         * @post Failure changes no accounting. Deferral records only the monotonic clock observation. Charges are never refunded,
         *       including pending/error polls. Equality with the target is admitted. Oversized mandatory units require a host loading
         * barrier.
         */
        [[nodiscard]] Result<bool> TryConsume(StreamingSchedulerLedgerId owner, std::uint64_t maximumNanoseconds,
                                              std::uint64_t elapsedNanoseconds);
        /** @brief Returns immutable owner/frame facts. @return Exact frame policy. */
        [[nodiscard]] StreamingOwnerFrameLimits Limits() const noexcept;
        /** @brief Returns conservative work already charged. @return Nanoseconds consumed. */
        [[nodiscard]] std::uint64_t ChargedNanoseconds() const noexcept;
        /** @brief Returns admitted work units, including pending/error polls. @return Units consumed. */
        [[nodiscard]] std::uint32_t ConsumedUnits() const noexcept;

    private:
        explicit StreamingOwnerFrameBudget(const StreamingOwnerFrameLimits &limits) noexcept;
        StreamingOwnerFrameLimits limits_;
        std::uint64_t chargedNanoseconds_{};
        std::uint64_t elapsedNanoseconds_{};
        std::uint32_t consumedUnits_{};
    };
}  // namespace Horo::WorldStreaming
