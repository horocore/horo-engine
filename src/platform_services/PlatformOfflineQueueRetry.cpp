#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"
#include "PlatformOfflineQueueImpl.h"

#include <algorithm>

namespace Horo::PlatformServices {
    /** @copydoc PlatformOfflineQueue::RetryUncommitted */
    Result<PlatformRequestMutation> PlatformOfflineQueue::RetryUncommitted(const PlatformOfflineOperationHandle operation,
                                                                           const TimePoint now, const std::uint32_t jitter,
                                                                           const std::chrono::seconds providerDelay) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<PlatformRequestMutation>::Failure(observed.ErrorValue());
        if (closed_)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::QueueUnavailable));
        auto *record = detail::FindOperation(operations_, operation, config_.generation);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::Stale));
        if (record->state != PlatformOfflineOperationState::Dispatching || providerDelay.count() < 0)
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidTransition));

        if (record->attempts >= config_.maximumAttempts)
            return CompleteOperation(*record, PlatformOfflineIntentState::PermanentlyFailed, now);

        // Integer capped doubling avoids overflow for both hostile hints and large attempt counts.
        auto delay = config_.initialRetryDelay;
        for (std::uint32_t attempt = 1; attempt < record->attempts; ++attempt)
            delay += std::min(delay, config_.maximumRetryDelay - delay);
        auto jittered = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
        const auto half = jittered / 2;
        const auto spread = static_cast<std::uint32_t>(half.count());
        jittered += std::chrono::milliseconds{spread == 0 ? 0 : jitter % (spread + 1)};
        const auto cap = std::chrono::duration_cast<std::chrono::milliseconds>(config_.maximumRetryDelay);
        const auto hint = std::chrono::duration_cast<std::chrono::milliseconds>(std::min(providerDelay, config_.maximumRetryDelay));
        jittered = std::min(cap, std::max(jittered, hint));
        const auto next = AddAge(now, jittered);
        if (!next.has_value())
            return Result<PlatformRequestMutation>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));

        record->state = PlatformOfflineOperationState::Pending;
        record->nextEligibleAt = next;
        for (auto &receipt : record->receipts)
            if (receipt.state == PlatformOfflineIntentState::Dispatching)
                receipt.state = PlatformOfflineIntentState::Pending;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }
}  // namespace Horo::PlatformServices
