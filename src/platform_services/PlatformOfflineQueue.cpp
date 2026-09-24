#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"
#include "PlatformOfflineQueueImpl.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::PlatformServices {
    /** @copydoc PlatformOfflineQueue::PlatformOfflineQueue */
    PlatformOfflineQueue::PlatformOfflineQueue(const PlatformOfflineQueueConfig &config) : config_(config) {}

    /** @copydoc PlatformOfflineQueue::~PlatformOfflineQueue */
    PlatformOfflineQueue::~PlatformOfflineQueue() = default;

    /** @copydoc PlatformOfflineQueue::ConfigurationIsValid */
    bool PlatformOfflineQueue::ConfigurationIsValid() const noexcept {
        return config_.activeCapacity != 0 && config_.activeCapacity <= PlatformOfflineQueueMaximumActiveIntents &&
               config_.perSubjectActiveCapacity != 0 &&
               config_.perSubjectActiveCapacity <= PlatformOfflineQueueMaximumActiveIntentsPerSubject &&
               config_.retainedCapacity >= config_.activeCapacity &&
               config_.retainedCapacity <= PlatformOfflineQueueMaximumRetainedIntents && config_.progressionMaximumAge.count() > 0 &&
               config_.progressionMaximumAge <= PlatformOfflineQueueMaximumProgressionAge && config_.presenceMaximumAge.count() > 0 &&
               config_.presenceMaximumAge < config_.progressionMaximumAge &&
               config_.presenceMaximumAge <= PlatformOfflineQueueMaximumPresenceAge && config_.terminalRetention.count() > 0 &&
               config_.terminalRetention <= PlatformOfflineQueueMaximumTerminalRetention && config_.producerRedeliveryHorizon.count() > 0 &&
               config_.producerRedeliveryHorizon <= config_.terminalRetention &&
               config_.producerRedeliveryHorizon <= PlatformOfflineQueueMaximumTerminalRetention &&
               config_.maximumPresenceDetailBytes <= PlatformOfflineQueueMaximumPresenceDetailBytes && config_.generation.IsValid();
    }

    /** @copydoc PlatformOfflineQueue::ObserveTime */
    Result<void> PlatformOfflineQueue::ObserveTime(const TimePoint now) {
        if (!ConfigurationIsValid())
            return Result<void>::Failure(MakeError(OfflineQueueErrors::InvalidConfiguration));
        if (lastObservedAt_.has_value() && now < *lastObservedAt_)
            return Result<void>::Failure(MakeError(OfflineQueueErrors::ClockMovedBackward));
        lastObservedAt_ = now;
        return Result<void>::Success();
    }

    /** @copydoc PlatformOfflineQueue::AddAge */
    std::optional<PlatformOfflineQueue::TimePoint> PlatformOfflineQueue::AddAge(const TimePoint time,
                                                                                const std::chrono::seconds age) noexcept {
        const auto delta = std::chrono::duration_cast<Clock::duration>(age);
        if (time.time_since_epoch() > Clock::duration::max() - delta)
            return std::nullopt;
        return TimePoint{time.time_since_epoch() + delta};
    }

    /** @copydoc PlatformOfflineQueue::Snapshot */
    PlatformOfflineOperationSnapshot PlatformOfflineQueue::Snapshot(const State &state) const {
        PlatformOfflineOperationSnapshot snapshot{.handle = state.handle,
                                                  .lane = state.lane,
                                                  .operation = state.operation,
                                                  .state = state.state};
        snapshot.receipts.reserve(state.receipts.size());
        for (const auto &receipt : state.receipts)
            snapshot.receipts.push_back(PlatformOfflineReceiptSnapshot{.id = receipt.intent.id,
                                                                       .state = receipt.state,
                                                                       .admittedAt = receipt.admittedAt,
                                                                       .expiresAt = receipt.expiresAt,
                                                                       .terminalAt = receipt.terminalAt});
        return snapshot;
    }

    /** @copydoc PlatformOfflineQueue::Query */
    Result<PlatformOfflineOperationSnapshot> PlatformOfflineQueue::Query(const PlatformOfflineIntentId id) const {
        if (!id.IsValid())
            return Result<PlatformOfflineOperationSnapshot>::Failure(MakeError(OfflineQueueErrors::InvalidIntent));
        const auto *operation = detail::FindReceipt(operations_, id);
        if (operation == nullptr)
            return Result<PlatformOfflineOperationSnapshot>::Failure(MakeError(OfflineQueueErrors::Expired));
        return Result<PlatformOfflineOperationSnapshot>::Success(Snapshot(*operation));
    }

    /** @copydoc PlatformOfflineQueue::PendingInLane */
    std::vector<PlatformOfflineOperationSnapshot> PlatformOfflineQueue::PendingInLane(const PlatformOfflineLaneKey &lane) const {
        std::vector<PlatformOfflineOperationSnapshot> pending;
        for (const auto &operation : operations_)
            if (operation.state == PlatformOfflineOperationState::Pending && operation.lane == lane)
                pending.push_back(Snapshot(operation));
        return pending;
    }

    /** @copydoc PlatformOfflineQueue::Expire */
    Result<std::vector<PlatformOfflineIntentId>> PlatformOfflineQueue::Expire(const TimePoint now) {
        if (const auto observed = ObserveTime(now); observed.HasError())
            return Result<std::vector<PlatformOfflineIntentId>>::Failure(observed.ErrorValue());
        std::vector<PlatformOfflineIntentId> expired;
        for (auto &operation : operations_)
            ExpireDue(operation, now, &expired);
        return Result<std::vector<PlatformOfflineIntentId>>::Success(std::move(expired));
    }

    /** @copydoc PlatformOfflineQueue::Generation */
    PlatformOfflineQueueGeneration PlatformOfflineQueue::Generation() const noexcept {
        return config_.generation;
    }

    /** @copydoc PlatformOfflineQueue::ActiveIntentCount */
    std::size_t PlatformOfflineQueue::ActiveIntentCount() const noexcept {
        return activeIntentCount_;
    }

    /** @copydoc PlatformOfflineQueue::RetainedIntentCount */
    std::size_t PlatformOfflineQueue::RetainedIntentCount() const noexcept {
        return retainedIntentCount_;
    }

    /** @copydoc PlatformOfflineQueue::OperationCount */
    std::size_t PlatformOfflineQueue::OperationCount() const noexcept {
        return operations_.size();
    }
}  // namespace Horo::PlatformServices
