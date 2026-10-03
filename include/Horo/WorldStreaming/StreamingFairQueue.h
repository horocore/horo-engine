#pragma once

/** @file StreamingFairQueue.h
 * @brief Authority-owned bounded priority queue with periodic oldest-admissible dispatch.
 */
#include "Horo/WorldStreaming/StreamingCellOperation.h"
#include "Horo/WorldStreaming/StreamingPriorityPolicy.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct StreamingFairQueueIdTag;
        struct StreamingFairQueueRevisionTag;
    }  // namespace Detail

    /** @brief Non-reused identity of one authority-owned queue lifetime. */
    using StreamingFairQueueId = Foundation::Detail::NonZeroId64<Detail::StreamingFairQueueIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Non-wrapping publication revision fencing queue commands and admission snapshots. */
    using StreamingFairQueueRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingFairQueueRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Mandatory bounded queue configuration; storage is allocated only at creation. */
    struct StreamingFairQueueRequest final {
        static constexpr std::uint32_t CurrentContractVersion = 1;
        static constexpr std::uint32_t MaximumEntries = 1'024;
        std::uint32_t contractVersion{CurrentContractVersion}; /**< Exact supported contract. */
        StreamingFairQueueId id{};                             /**< Non-reused lifetime identity issued by the host. */
        WorldPartitionId partition{};                          /**< Owning mounted partition. */
        PartitionEpoch epoch{};                                /**< Exact mounted incarnation. */
        std::uint32_t maximumEntries{64};                      /**< Positive storage ceiling. */
        std::uint32_t maximumPriorityDispatches{3};            /**< Score-first successes between oldest-first successes; range [1,1024]. */
    };

    /** @brief Exact command fence and unscaled monotonic service time. */
    struct StreamingFairQueueContext final {
        StreamingFairQueueId owner{};
        StreamingFairQueueRevision revision{};
        std::uint64_t serviceTimeMilliseconds{};
    };

    /** @brief Value-owned pending operation and source evidence; enqueue time is captured by the queue. */
    struct StreamingFairQueueEntry final {
        StreamingCellOperationHandle operation{};
        StreamingCellPriorityCandidate priority{};
    };
    /** @brief Host budget/required-content decision; deferred work never consumes dispatch credit. */
    enum class StreamingFairQueueEligibility : std::uint8_t {
        Admissible,
        Deferred
    };

    /** @brief One exact pending operation's eligibility in the context's complete queue snapshot. */
    struct StreamingFairQueueAdmission final {
        StreamingCellOperationHandle operation{};
        StreamingFairQueueEligibility eligibility{StreamingFairQueueEligibility::Deferred};
    };
    /** @brief Transparent policy explaining the selected operation. */
    enum class StreamingFairQueueDispatchReason : std::uint8_t {
        Priority,
        OldestAdmissible
    };

    /** @brief Fenced proposal; retained work leaves the queue only after the host commits actual admission. */
    struct StreamingFairQueueSelection final {
        StreamingFairQueueId owner{};
        StreamingFairQueueRevision revision{};
        StreamingCellOperationHandle operation{};
        StreamingFairQueueDispatchReason reason{StreamingFairQueueDispatchReason::Priority};
        [[nodiscard]] constexpr auto operator<=>(const StreamingFairQueueSelection &) const noexcept = default;
    };

    /**
     * @brief Single StreamingAuthorityRole owner of pending work, fairness credit and admission proposals.
     * @details No locks, I/O, callbacks, reservations or cell-state changes. Creation allocates bounded storage;
     *          successful later commands allocate nothing; typed error diagnostics use the Foundation error model. Only successful
     * CommitDispatch advances fairness credit. After at most maximumPriorityDispatches score-first commits, the oldest admissible entry
     * wins. With N older continuously admissible entries a pending entry progresses within (N+1)*(maximumPriorityDispatches+1) successful
     * dispatches; new arrivals cannot overtake it in fair slots. This is a service bound, not a wall-clock or budget-bypass guarantee. The
     * owner supplies complete current eligibility under the same safe point as budget admission. Failed admission leaves the proposal
     * queued; selecting again replaces only the proposal. Shutdown/discard release pending metadata only: admitted resources belong to the
     * scheduler ledger.
     */
    class StreamingFairQueue final {
    public:
        /** @brief Validates and preallocates the queue. @param request Exact owner and bounds.
         * @param policy Immutable numerical priority policy. @return Queue or typed invalid/unsupported/capacity failure. */
        [[nodiscard]] static Result<StreamingFairQueue> Create(const StreamingFairQueueRequest &request, StreamingPriorityPolicy policy);
        StreamingFairQueue(const StreamingFairQueue &) = delete;
        StreamingFairQueue &operator=(const StreamingFairQueue &) = delete;
        StreamingFairQueue(StreamingFairQueue &&) noexcept = default;
        StreamingFairQueue &operator=(StreamingFairQueue &&) noexcept = default;
        /** @brief Reads the exact queue publication. @return Current non-zero revision. */
        [[nodiscard]] StreamingFairQueueRevision Revision() const noexcept;
        /** @brief Reads pending work count. @return Number of retained entries. */
        [[nodiscard]] std::size_t Size() const noexcept;
        /** @brief Reads the canonical pending snapshot for authority-owned eligibility evaluation.
         * @return Borrowed immutable entries, valid until the next queue mutation or destruction. */
        [[nodiscard]] std::span<const StreamingFairQueueEntry> PendingEntries() const noexcept;
        /** @brief Reads lifecycle state. @return True after terminal shutdown. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Enqueues unique cell work and captures service-time age. @param context Current fence/time.
         * @param entry Exact operation and source evidence. @return Success or atomic typed failure. */
        [[nodiscard]] Result<void> Enqueue(const StreamingFairQueueContext &context, StreamingFairQueueEntry entry);
        /** @brief Replaces pending work for the same cell with a strictly newer attempt, preserving wait order.
         * @param context Current fence/time. @param expected Exact old operation.
         * @param replacement Successor operation/source evidence. @return Success or atomic typed failure. */
        [[nodiscard]] Result<void> Replace(const StreamingFairQueueContext &context, const StreamingCellOperationHandle &expected,
                                           StreamingFairQueueEntry replacement);
        /** @brief Proposes eligible work without removing it or consuming fairness credit.
         * @param context Current fence/time. @param admission Exactly one eligibility row per pending operation, in any order.
         * @return Empty if all deferred; otherwise a revision-fenced proposal, or atomic typed failure. */
        [[nodiscard]] Result<std::optional<StreamingFairQueueSelection>> Select(const StreamingFairQueueContext &context,
                                                                                std::span<const StreamingFairQueueAdmission> admission);
        /** @brief Removes only the current proposal after successful host admission, advancing fairness credit.
         * @param context Current fence/time. @param selection Exact most recent proposal.
         * @return Success or atomic stale/invalid/lifecycle failure. */
        [[nodiscard]] Result<void> CommitDispatch(const StreamingFairQueueContext &context, const StreamingFairQueueSelection &selection);
        /** @brief Removes exact queued work on cancellation, failure or explicit replacement withdrawal.
         * @param context Current fence/time. @param expected Exact operation. @param outcome Cancelled, Failed or Replaced.
         * @return Success or atomic typed failure; no admitted resource credit is released. */
        [[nodiscard]] Result<void> Discard(const StreamingFairQueueContext &context, const StreamingCellOperationHandle &expected,
                                           StreamingCellOperationOutcome outcome);
        /** @brief Closes and clears pending work idempotently. @param context Current fence/time.
         * @return Success or atomic typed failure. Old queue lifetime cannot admit a replacement partition. */
        [[nodiscard]] Result<void> Shutdown(const StreamingFairQueueContext &context);

    private:
        StreamingFairQueue(const StreamingFairQueueRequest &request, StreamingPriorityPolicy policy);
        [[nodiscard]] Result<void> ValidateContext(const StreamingFairQueueContext &context, bool allowClosed = false) const;
        [[nodiscard]] Result<void> ValidateEntry(const StreamingFairQueueEntry &entry, std::uint64_t time) const;
        [[nodiscard]] std::size_t Find(const StreamingCellOperationHandle &operation) const noexcept;
        [[nodiscard]] Result<void> ValidateAdmission(std::span<const StreamingFairQueueAdmission> admission) const;
        [[nodiscard]] std::size_t SelectIndex(bool fair) const noexcept;
        void Publish(std::uint64_t time) noexcept;
        StreamingFairQueueRequest request_;
        StreamingPriorityPolicy policy_;
        StreamingFairQueueRevision revision_;
        std::uint64_t serviceTime_{};
        std::uint32_t priorityDispatches_{};
        bool closed_{};
        std::vector<StreamingFairQueueEntry> entries_;
        std::vector<StreamingCellPriorityCandidate> candidates_;
        std::vector<StreamingRankedCellPriority> ranked_;
        std::vector<std::uint8_t> eligible_;
        std::optional<StreamingFairQueueSelection> pending_;
    };
}  // namespace Horo::WorldStreaming
