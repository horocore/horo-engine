#include "Horo/WorldStreaming/StreamingFairQueue.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using Internal::Failure;
        using namespace WorldStreamingErrors;

        /** @brief Constructs exact priority evidence from this queue's owned policy and partition. */
        [[nodiscard]] StreamingPriorityEvaluationContext PriorityContext(const StreamingFairQueueRequest &request,
                                                                         const StreamingPriorityPolicy &policy, const std::uint64_t time) {
            return {policy.Id(), policy.Revision(), request.partition, request.epoch, time, StreamingPriorityPolicyState::Active};
        }
    }  // namespace

    StreamingFairQueue::StreamingFairQueue(const StreamingFairQueueRequest &request, StreamingPriorityPolicy policy)
        : request_(request), policy_(std::move(policy)), revision_(StreamingFairQueueRevision::Create(1).Value()) {
        entries_.reserve(request.maximumEntries);
        candidates_.reserve(request.maximumEntries);
        ranked_.resize(request.maximumEntries);
        eligible_.resize(request.maximumEntries);
    }

    /** @copydoc StreamingFairQueue::Create */
    Result<StreamingFairQueue> StreamingFairQueue::Create(const StreamingFairQueueRequest &request, StreamingPriorityPolicy policy) {
        if (request.contractVersion != StreamingFairQueueRequest::CurrentContractVersion)
            return Failure<StreamingFairQueue>(FairQueueUnsupported);
        if (!request.id.IsValid() || !request.partition.IsValid() || !request.epoch.IsValid() || request.maximumEntries == 0 ||
            request.maximumPriorityDispatches == 0)
            return Failure<StreamingFairQueue>(FairQueueInvalid);
        if (request.maximumEntries > StreamingFairQueueRequest::MaximumEntries || request.maximumEntries > policy.MaximumCandidates() ||
            request.maximumPriorityDispatches > StreamingFairQueueRequest::MaximumEntries)
            return Failure<StreamingFairQueue>(FairQueueCapacityExceeded);
        return Result<StreamingFairQueue>::Success(StreamingFairQueue{request, std::move(policy)});
    }

    /** @copydoc StreamingFairQueue::Revision */
    StreamingFairQueueRevision StreamingFairQueue::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc StreamingFairQueue::Size */
    std::size_t StreamingFairQueue::Size() const noexcept {
        return entries_.size();
    }

    /** @copydoc StreamingFairQueue::PendingEntries */
    std::span<const StreamingFairQueueEntry> StreamingFairQueue::PendingEntries() const noexcept {
        return entries_;
    }

    /** @copydoc StreamingFairQueue::IsClosed */
    bool StreamingFairQueue::IsClosed() const noexcept {
        return closed_;
    }

    Result<void> StreamingFairQueue::ValidateContext(const StreamingFairQueueContext &context, const bool allowClosed) const {
        if (!context.owner.IsValid() || !context.revision.IsValid())
            return Failure<void>(FairQueueInvalid);
        if (context.owner != request_.id || context.revision != revision_ || context.serviceTimeMilliseconds < serviceTime_)
            return Failure<void>(FairQueueStale);
        if (closed_ && !allowClosed)
            return Failure<void>(FairQueueLifecycleUnavailable);
        if (revision_.Value() == std::numeric_limits<std::uint64_t>::max() && !closed_)
            return Failure<void>(GenerationExhausted);
        return Result<void>::Success();
    }

    Result<void> StreamingFairQueue::ValidateEntry(const StreamingFairQueueEntry &entry, const std::uint64_t time) const {
        if (!entry.operation.IsValid() || entry.operation.fence.cell != entry.priority.cell)
            return Failure<void>(FairQueueInvalid);
        if (entry.operation.fence.partition != request_.partition || entry.operation.fence.epoch != request_.epoch)
            return Failure<void>(FairQueueStale);
        const std::array candidate{entry.priority};
        std::array<StreamingRankedCellPriority, 1> ranked{};
        if (const auto valid = RankStreamingCellPriorities(policy_, PriorityContext(request_, policy_, time), candidate, ranked);
            valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        return Result<void>::Success();
    }

    std::size_t StreamingFairQueue::Find(const StreamingCellOperationHandle &operation) const noexcept {
        const auto found = std::ranges::find(entries_, operation, &StreamingFairQueueEntry::operation);
        return static_cast<std::size_t>(found - entries_.begin());
    }

    void StreamingFairQueue::Publish(const std::uint64_t time) noexcept {
        revision_ = StreamingFairQueueRevision::Create(revision_.Value() + 1).Value();
        serviceTime_ = time;
        pending_.reset();
    }

    /** @copydoc StreamingFairQueue::Enqueue */
    Result<void> StreamingFairQueue::Enqueue(const StreamingFairQueueContext &context, StreamingFairQueueEntry entry) {
        if (const auto valid = ValidateContext(context); valid.HasError())
            return valid;
        if (const auto valid = ValidateEntry(entry, context.serviceTimeMilliseconds); valid.HasError())
            return valid;
        if (std::ranges::any_of(entries_, [&](const auto &other) {
            return other.operation.operation == entry.operation.operation || other.priority.cell == entry.priority.cell;
        }))
            return Failure<void>(FairQueueIdentityConflict);
        if (entries_.size() == request_.maximumEntries)
            return Failure<void>(FairQueueCapacityExceeded);
        entry.priority.queuedAtServiceMilliseconds = context.serviceTimeMilliseconds;
        entries_.push_back(entry);
        Publish(context.serviceTimeMilliseconds);
        return Result<void>::Success();
    }

    /** @copydoc StreamingFairQueue::Replace */
    Result<void> StreamingFairQueue::Replace(const StreamingFairQueueContext &context, const StreamingCellOperationHandle &expected,
                                             StreamingFairQueueEntry replacement) {
        if (const auto valid = ValidateContext(context); valid.HasError())
            return valid;
        if (!expected.IsValid())
            return Failure<void>(FairQueueInvalid);
        const auto index = Find(expected);
        if (index == entries_.size())
            return Failure<void>(FairQueueStale);
        if (const auto valid = ValidateEntry(replacement, context.serviceTimeMilliseconds); valid.HasError())
            return valid;
        if (replacement.priority.cell != expected.fence.cell || replacement.operation.fence.generation <= expected.fence.generation ||
            replacement.operation.operation == expected.operation)
            return Failure<void>(FairQueueStale);
        if (std::ranges::any_of(entries_, [&](const auto &other) {
            return other.operation.operation == replacement.operation.operation;
        }))
            return Failure<void>(FairQueueIdentityConflict);
        replacement.priority.queuedAtServiceMilliseconds = entries_[index].priority.queuedAtServiceMilliseconds;
        entries_[index] = replacement;
        Publish(context.serviceTimeMilliseconds);
        return Result<void>::Success();
    }

    Result<void> StreamingFairQueue::ValidateAdmission(const std::span<const StreamingFairQueueAdmission> admission) const {
        if (admission.size() > request_.maximumEntries)
            return Failure<void>(FairQueueCapacityExceeded);
        if (admission.size() != entries_.size())
            return Failure<void>(FairQueueInvalid);
        for (std::size_t index = 0; index < admission.size(); ++index) {
            const auto &row = admission[index];
            if (!row.operation.IsValid())
                return Failure<void>(FairQueueInvalid);
            if (row.eligibility != StreamingFairQueueEligibility::Admissible && row.eligibility != StreamingFairQueueEligibility::Deferred)
                return Failure<void>(FairQueueUnsupported);
            if (Find(row.operation) == entries_.size())
                return Failure<void>(FairQueueStale);
            for (std::size_t prior = 0; prior < index; ++prior)
                if (admission[prior].operation == row.operation)
                    return Failure<void>(FairQueueIdentityConflict);
        }
        return Result<void>::Success();
    }

    /** @copydoc StreamingFairQueue::Select */
    Result<std::optional<StreamingFairQueueSelection>> StreamingFairQueue::Select(
        const StreamingFairQueueContext &context, const std::span<const StreamingFairQueueAdmission> admission) {
        using SelectionResult = Result<std::optional<StreamingFairQueueSelection>>;
        if (const auto valid = ValidateContext(context); valid.HasError())
            return SelectionResult::Failure(valid.ErrorValue());
        if (const auto valid = ValidateAdmission(admission); valid.HasError())
            return SelectionResult::Failure(valid.ErrorValue());
        std::ranges::fill(eligible_, std::uint8_t{0});
        for (const auto &row : admission)
            eligible_[Find(row.operation)] = row.eligibility == StreamingFairQueueEligibility::Admissible;
        candidates_.clear();
        for (std::size_t index = 0; index < entries_.size(); ++index)
            if (eligible_[index])
                candidates_.push_back(entries_[index].priority);
        if (const auto ranked = RankStreamingCellPriorities(policy_, PriorityContext(request_, policy_, context.serviceTimeMilliseconds),
                                                            candidates_, ranked_);
            ranked.HasError())
            return SelectionResult::Failure(ranked.ErrorValue());
        if (candidates_.empty()) {
            // A fresh all-deferred snapshot revokes an older proposal without spending fairness credit.
            Publish(context.serviceTimeMilliseconds);
            return SelectionResult::Success(std::nullopt);
        }
        const bool fair = priorityDispatches_ == request_.maximumPriorityDispatches;
        const auto selected = SelectIndex(fair);
        // Entries preserve insertion order across same-time arrivals and attempt replacement.
        Publish(context.serviceTimeMilliseconds);
        pending_ = StreamingFairQueueSelection{request_.id, revision_, entries_[selected].operation,
                                               fair ? StreamingFairQueueDispatchReason::OldestAdmissible
                                                    : StreamingFairQueueDispatchReason::Priority};
        return SelectionResult::Success(pending_);
    }

    std::size_t StreamingFairQueue::SelectIndex(const bool fair) const noexcept {
        std::size_t selected = entries_.size();
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (!eligible_[index])
                continue;
            if (!fair) {
                if (entries_[index].priority.cell == ranked_[0].candidate.cell) {
                    selected = index;
                    break;
                }
            } else if (selected == entries_.size() ||
                       entries_[index].priority.queuedAtServiceMilliseconds < entries_[selected].priority.queuedAtServiceMilliseconds) {
                selected = index;
            }
        }
        return selected;
    }

    /** @copydoc StreamingFairQueue::CommitDispatch */
    Result<void> StreamingFairQueue::CommitDispatch(const StreamingFairQueueContext &context,
                                                    const StreamingFairQueueSelection &selection) {
        if (const auto valid = ValidateContext(context); valid.HasError())
            return valid;
        if (!pending_ || selection != *pending_)
            return Failure<void>(FairQueueStale);
        const auto index = Find(selection.operation);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
        priorityDispatches_ = selection.reason == StreamingFairQueueDispatchReason::OldestAdmissible ? 0 : priorityDispatches_ + 1;
        Publish(context.serviceTimeMilliseconds);
        return Result<void>::Success();
    }

    /** @copydoc StreamingFairQueue::Discard */
    Result<void> StreamingFairQueue::Discard(const StreamingFairQueueContext &context, const StreamingCellOperationHandle &expected,
                                             const StreamingCellOperationOutcome outcome) {
        if (const auto valid = ValidateContext(context); valid.HasError())
            return valid;
        if (!expected.IsValid())
            return Failure<void>(FairQueueInvalid);
        if (outcome != StreamingCellOperationOutcome::Cancelled && outcome != StreamingCellOperationOutcome::Failed &&
            outcome != StreamingCellOperationOutcome::Replaced)
            return Failure<void>(FairQueueUnsupported);
        const auto index = Find(expected);
        if (index == entries_.size())
            return Failure<void>(FairQueueStale);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
        Publish(context.serviceTimeMilliseconds);
        return Result<void>::Success();
    }

    /** @copydoc StreamingFairQueue::Shutdown */
    Result<void> StreamingFairQueue::Shutdown(const StreamingFairQueueContext &context) {
        if (const auto valid = ValidateContext(context, true); valid.HasError())
            return valid;
        if (!closed_) {
            entries_.clear();
            Publish(context.serviceTimeMilliseconds);
            closed_ = true;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::WorldStreaming
