#pragma once

/**
 * @file StreamingCellActivation.h
 * @brief Transactional publication of prepared cell resources at the Scene safe point.
 */

#include "Horo/WorldStreaming/StreamingOwnerFrameBudget.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag separating activation transaction identities from operation and service identities. */
        struct StreamingCellActivationIdTag;
    }  // namespace Detail

    /** @brief Stable process-local identity for one prepared cell activation transaction. */
    using StreamingCellActivationId =
        Foundation::Detail::NonZeroId64<Detail::StreamingCellActivationIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Owner phase supplied when a prepared activation is committed. */
    enum class StreamingCellActivationCommitPoint : std::uint8_t {
        PreUpdate,
        CommitDeferredLifecycleChanges,
        PostUpdate,
        Count
    };

    /** @brief Admission lifecycle of the activation authority. */
    enum class StreamingCellActivationLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count
    };

    /** @brief Observable terminal ownership state of a prepared transaction. */
    enum class StreamingCellActivationState : std::uint8_t {
        Prepared,
        Published,
        RolledBack
    };

    /** @brief Stable identity and immutable revision required from one publication participant. */
    struct StreamingCellActivationRequirement final {
        StreamingRuntimeServiceId participant{};    /**< Stable Scene or feature-adapter identity. */
        StreamingRuntimeServiceRevision revision{}; /**< Exact prepared service revision. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingCellActivationRequirement &) const noexcept = default;
    };

    /**
     * @brief Unique prepared receipt owned by one Scene or feature-provider publication participant.
     * @details Implementations retain private/native resources behind this Horo-owned boundary. Publication is a bounded no-fail
     *          ownership transfer; rollback is idempotent and releases the exact prepared receipt without touching live Scene state.
     */
    class IStreamingCellActivationReceipt {
    public:
        /** @brief Releases the receipt implementation after publication or rollback. */
        virtual ~IStreamingCellActivationReceipt() = default;
        IStreamingCellActivationReceipt(const IStreamingCellActivationReceipt &) = delete;
        IStreamingCellActivationReceipt &operator=(const IStreamingCellActivationReceipt &) = delete;

        /** @brief Returns stable participant identity and revision. @return Immutable requirement represented by this receipt. */
        [[nodiscard]] virtual StreamingCellActivationRequirement Requirement() const noexcept = 0;
        /** @brief Returns the exact operation and generation fence prepared by this receipt. @return Immutable operation handle. */
        [[nodiscard]] virtual StreamingCellOperationHandle Operation() const noexcept = 0;
        /** @brief Returns a positive immutable upper bound for publication. @return Owner-work nanoseconds; the sum must cover the complete
         * no-fail unit and scheduler finalization. */
        [[nodiscard]] virtual std::uint64_t MaximumPublicationNanoseconds() const noexcept = 0;
        /** @brief Publishes already-prepared state without allocation, waiting, or failure within the declared unit bound. */
        virtual void PublishPrepared() noexcept = 0;
        /** @brief Revokes/transfers uncommitted state to the admitted retirement owner; repeated calls must be harmless.
         * @details This bounded no-fail control operation must not synchronously destroy heavy resources. Their destruction
         *          remains with the same ordered retirement participants and their shared owner-frame budget. */
        virtual void RollbackPrepared() noexcept = 0;

    protected:
        IStreamingCellActivationReceipt() = default;
    };

    /** @brief Bounded construction facts for one exact activation attempt. */
    struct StreamingCellActivationContext final {
        StreamingCellActivationId activation{}; /**< Unique activation transaction identity. */
        StreamingCellOperation operation;       /**< Exact Activate operation in Activating phase. */
        std::size_t maximumReceipts{};          /**< Positive hard ceiling for required receipts. */
        StreamingCellActivationLifecycle lifecycle{StreamingCellActivationLifecycle::Closed}; /**< Admission state. */
        StreamingSchedulerLedgerId scheduler; /**< Exact scheduler lifetime supplying shared owner-work admission. */
    };

    /** @brief Move-only owner of a complete required receipt set and its atomic publication decision. */
    class StreamingCellActivationTransaction final {
    public:
        /** @brief Rolls back every receipt when prepared ownership was not published. */
        ~StreamingCellActivationTransaction();
        StreamingCellActivationTransaction(const StreamingCellActivationTransaction &) = delete;
        StreamingCellActivationTransaction &operator=(const StreamingCellActivationTransaction &) = delete;
        /** @brief Transfers unique prepared-receipt ownership. @param other Source transaction. */
        StreamingCellActivationTransaction(StreamingCellActivationTransaction &&other) noexcept;
        /** @brief Rolls back current ownership before transfer. @param other Source transaction. @return This transaction. */
        StreamingCellActivationTransaction &operator=(StreamingCellActivationTransaction &&other) noexcept;

        /**
         * @brief Adopts exactly one prepared receipt for every required participant.
         * @param context Exact activation identity, operation fence, lifecycle and receipt ceiling.
         * @param required Complete canonical required Scene/provider set.
         * @param receipts Unique prepared receipts; ownership is consumed on success or rollback failure.
         * @return Prepared transaction or typed invalid, stale, incomplete, capacity or lifecycle failure.
         * @post Every supplied receipt is rolled back on failure.
         */
        [[nodiscard]] static Result<StreamingCellActivationTransaction> Prepare(
            const StreamingCellActivationContext &context, std::span<const StreamingCellActivationRequirement> required,
            std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts);

        /**
         * @brief Publishes the complete prepared set at the exact Scene structural safe point.
         * @param expected Exact current operation snapshot and generation fence.
         * @param commitPoint Current owner phase; only CommitDeferredLifecycleChanges is accepted.
         * @param lifecycle Current authority lifecycle; cancellation/shutdown rolls the transaction back.
         * @param budget Unique shared budget for this owner frame; never a new per-cell budget.
         * @param elapsedNanoseconds Monotonic elapsed service time sampled immediately before this call.
         * @return Success or typed stale, safe-point, lifecycle, frame-deferred or oversized-unit failure.
         * @details Frame deferral retains the complete prepared set and publishes nothing. A cell's publication is indivisible;
         *          detached preparation must make that final transfer bounded rather than publishing half a cell across frames.
         * @post Success publishes every receipt once in canonical participant order. Failure after a stale/lifecycle check rolls all back.
         */
        [[nodiscard]] Result<void> Commit(const StreamingCellOperation &expected, StreamingCellActivationCommitPoint commitPoint,
                                          StreamingCellActivationLifecycle lifecycle, StreamingOwnerFrameBudget &budget,
                                          std::uint64_t elapsedNanoseconds);

        /** @brief Explicitly rolls back every prepared receipt in reverse publication order; idempotent. */
        void Rollback() noexcept;

        /** @brief Returns the exact activation identity. @return Stable transaction identity. */
        [[nodiscard]] StreamingCellActivationId Id() const noexcept;
        /** @brief Returns the exact operation and generation fence. @return Immutable operation handle. */
        [[nodiscard]] const StreamingCellOperationHandle &Operation() const noexcept;
        /** @brief Returns current ownership state. @return Prepared, Published or RolledBack. */
        [[nodiscard]] StreamingCellActivationState State() const noexcept;
        /** @brief Returns the complete canonical required set. @return Borrow valid for this transaction lifetime. */
        [[nodiscard]] std::span<const StreamingCellActivationRequirement> Requirements() const noexcept;

    private:
        StreamingCellActivationTransaction(const StreamingCellActivationContext &context,
                                           std::vector<StreamingCellActivationRequirement> requirements,
                                           std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts,
                                           std::uint64_t publicationNanoseconds) noexcept;

        StreamingCellActivationContext context_;
        std::vector<StreamingCellActivationRequirement> requirements_;
        std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts_;
        std::uint64_t publicationNanoseconds_{};
        StreamingCellActivationState state_{StreamingCellActivationState::RolledBack};
    };
}  // namespace Horo::WorldStreaming
