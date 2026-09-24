#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"
#include "PlatformOfflineQueueImpl.h"

#include <algorithm>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        [[nodiscard]] bool IsActiveReceipt(const PlatformOfflineIntentState state) noexcept {
            using enum PlatformOfflineIntentState;
            return state == Pending || state == Dispatching || state == Reconciling || state == Suspended;
        }

        template <typename ReceiptRange>
        void TransitionReceipts(ReceiptRange &receipts, const PlatformOfflineIntentState from, const PlatformOfflineIntentState to) {
            for (auto &receipt : receipts)
                if (receipt.state == from)
                    receipt.state = to;
        }
    }  // namespace

    /** @copydoc PlatformOfflineQueue::MarkDispatching */
    Result<PlatformRequestMutation> PlatformOfflineQueue::MarkDispatching(const PlatformOfflineOperationHandle operation,
                                                                          const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Dispatching)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        for (const auto &candidate : operations_)
            if (candidate.lane == record->lane && candidate.handle.sequence <= record->handle.sequence &&
                (candidate.state == PlatformOfflineOperationState::Pending ||
                 candidate.state == PlatformOfflineOperationState::Suspended) &&
                !ReceiptsRemainLive(candidate, now))
                return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Expired));
        if (record->state != PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        if (const auto earlierLaneWork = std::ranges::any_of(operations_,
                                                             [record](const State &candidate) {
            return candidate.lane == record->lane && candidate.handle.sequence < record->handle.sequence &&
                   candidate.state != PlatformOfflineOperationState::Terminal;
        });
            earlierLaneWork)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        record->state = PlatformOfflineOperationState::Dispatching;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Pending)
                receipt.state = PlatformOfflineIntentState::Dispatching;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::MarkReconciling */
    Result<PlatformRequestMutation> PlatformOfflineQueue::MarkReconciling(const PlatformOfflineOperationHandle operation) {
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Reconciling)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Dispatching)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        record->state = PlatformOfflineOperationState::Reconciling;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Dispatching)
                receipt.state = PlatformOfflineIntentState::Reconciling;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::SuspendPending */
    Result<PlatformRequestMutation> PlatformOfflineQueue::SuspendPending(const PlatformOfflineOperationHandle operation) {
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Suspended)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        record->state = PlatformOfflineOperationState::Suspended;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Pending)
                receipt.state = PlatformOfflineIntentState::Suspended;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::CancelPending */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CancelPending(const PlatformOfflineOperationHandle operation,
                                                                        const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Terminal) {
            const auto hasAbandoned = std::ranges::any_of(record->receipts, [](const State::Receipt &receipt) {
                return receipt.state == PlatformOfflineIntentState::Abandoned;
            });
            if (const auto onlyAbandonedOrExpired = std::ranges::all_of(record->receipts,
                                                                        [](const State::Receipt &receipt) {
                return receipt.state == PlatformOfflineIntentState::Abandoned || receipt.state == PlatformOfflineIntentState::Expired;
            });
                hasAbandoned && onlyAbandonedOrExpired)
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            const auto allExpired = std::ranges::all_of(record->receipts, [](const State::Receipt &receipt) {
                return receipt.state == PlatformOfflineIntentState::Expired;
            });
            return Result<PlatformRequestMutation>::Failure(
                MakeError(allExpired ? OfflineQueueErrors::Expired : OfflineQueueErrors::InvalidTransition));
        }
        if (record->state != PlatformOfflineOperationState::Pending && record->state != PlatformOfflineOperationState::Suspended)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        if (!ReceiptsRemainLive(*record, now))
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Expired));
        for (auto &receipt : record->receipts) {
            if (!IsActiveReceipt(receipt.state))
                continue;
            receipt.state = PlatformOfflineIntentState::Abandoned;
            receipt.terminalAt = now;
            --activeIntentCount_;
        }
        record->state = PlatformOfflineOperationState::Terminal;
        record->terminalAt = now;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::Resume */
    Result<PlatformRequestMutation> PlatformOfflineQueue::Resume(const PlatformOfflineOperationHandle operation, const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state == PlatformOfflineOperationState::Pending)
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (record->state != PlatformOfflineOperationState::Suspended)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        if (!ReceiptsRemainLive(*record, now))
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Expired));
        record->state = PlatformOfflineOperationState::Pending;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Suspended)
                receipt.state = PlatformOfflineIntentState::Pending;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::CompleteOperation */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CompleteOperation(State &record, const PlatformOfflineIntentState terminal,
                                                                            const TimePoint now) {
        if (record.state == PlatformOfflineOperationState::Terminal) {
            const auto hasOutcome = std::ranges::any_of(record.receipts, [terminal](const State::Receipt &receipt) {
                return receipt.state == terminal;
            });
            const auto sameOutcome = std::ranges::all_of(record.receipts, [terminal](const State::Receipt &receipt) {
                return receipt.state == terminal || receipt.state == PlatformOfflineIntentState::Expired;
            });
            return hasOutcome && sameOutcome ? Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged)
                                             : Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));
        }
        if (record.state != PlatformOfflineOperationState::Dispatching && record.state != PlatformOfflineOperationState::Reconciling)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        for (auto &receipt : record.receipts) {
            if (!IsActiveReceipt(receipt.state))
                continue;
            receipt.state = terminal;
            receipt.terminalAt = now;
            --activeIntentCount_;
        }
        record.state = PlatformOfflineOperationState::Terminal;
        record.terminalAt = now;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformOfflineQueue::CompleteSuccess */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CompleteSuccess(const PlatformOfflineOperationHandle operation,
                                                                          const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        return CompleteOperation(*record, PlatformOfflineIntentState::Succeeded, now);
    }

    /** @copydoc PlatformOfflineQueue::CompletePermanentlyFailed */
    Result<PlatformRequestMutation> PlatformOfflineQueue::CompletePermanentlyFailed(const PlatformOfflineOperationHandle operation,
                                                                                    const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        return CompleteOperation(*record, PlatformOfflineIntentState::PermanentlyFailed, now);
    }

    /** @copydoc PlatformOfflineQueue::Shutdown */
    Result<std::vector<PlatformOfflineIntentId>> PlatformOfflineQueue::Shutdown(const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<std::vector<PlatformOfflineIntentId>>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<std::vector<PlatformOfflineIntentId>>::Success({});

        std::vector<PlatformOfflineIntentId> expired;
        expired.reserve(DueReceiptCount(now));
        for (auto &operation : operations_) {
            ExpireDue(operation, now, &expired);
            if (operation.state == PlatformOfflineOperationState::Pending) {
                operation.state = PlatformOfflineOperationState::Suspended;
                TransitionReceipts(operation.receipts, PlatformOfflineIntentState::Pending, PlatformOfflineIntentState::Suspended);
            } else if (operation.state == PlatformOfflineOperationState::Dispatching) {
                operation.state = PlatformOfflineOperationState::Reconciling;
                TransitionReceipts(operation.receipts, PlatformOfflineIntentState::Dispatching, PlatformOfflineIntentState::Reconciling);
            }
        }
        closed_ = true;
        return Result<std::vector<PlatformOfflineIntentId>>::Success(std::move(expired));
    }

    /** @copydoc PlatformOfflineQueue::Compact */
    Result<std::vector<PlatformOfflineIntentId>> PlatformOfflineQueue::Compact(const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<std::vector<PlatformOfflineIntentId>>::Failure(observed.ErrorValue());

        std::vector<PlatformOfflineIntentId> compacted;
        auto operation = operations_.begin();
        while (operation != operations_.end()) {
            if (operation->state != PlatformOfflineOperationState::Terminal || !operation->terminalAt.has_value()) {
                ++operation;
                continue;
            }
            if (const auto retentionEndsAt = AddAge(*operation->terminalAt, config_.terminalRetention);
                !retentionEndsAt.has_value() || now < *retentionEndsAt) {
                ++operation;
                continue;
            }
            for (const auto &receipt : operation->receipts)
                compacted.push_back(receipt.intent.id);
            retainedIntentCount_ -= operation->receipts.size();
            operation = operations_.erase(operation);
        }
        return Result<std::vector<PlatformOfflineIntentId>>::Success(std::move(compacted));
    }
}  // namespace Horo::PlatformServices
