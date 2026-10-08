#include "Horo/PlatformServices/PlatformRequest.h"

#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "PlatformRequestState.h"
#include "PlatformServicesMetrics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    namespace {
        /** @brief Publishes the bounded terminal-state counter after authoritative state mutation succeeds. */
        void RecordTerminalMetric(const PlatformRequestState terminalState) {
            using enum Detail::PlatformRequestMetricOutcome;
            Detail::PlatformRequestMetricOutcome outcome = Failed;
            switch (terminalState) {
                case PlatformRequestState::Succeeded:
                    outcome = Succeeded;
                    break;
                case PlatformRequestState::Cancelled:
                    outcome = Cancelled;
                    break;
                case PlatformRequestState::TimedOut:
                    outcome = TimedOut;
                    break;
                case PlatformRequestState::Failed:
                    outcome = Failed;
                    break;
                case PlatformRequestState::Queued:
                case PlatformRequestState::Running:
                case PlatformRequestState::Cancelling:
                    break;
            }
            Detail::RecordPlatformRequestMetric(outcome);
        }
    }  // namespace

    /** @copydoc PlatformRequestSubscription::~PlatformRequestSubscription */
    PlatformRequestSubscription::~PlatformRequestSubscription() {
        Reset();
    }

    /** @copydoc PlatformRequestSubscription::PlatformRequestSubscription */
    PlatformRequestSubscription::PlatformRequestSubscription(PlatformRequestSubscription &&other) noexcept
        : slot_(std::move(other.slot_)) {}

    /** @copydoc PlatformRequestSubscription::operator= */
    PlatformRequestSubscription &PlatformRequestSubscription::operator=(PlatformRequestSubscription &&other) noexcept {
        if (this != &other) {
            Reset();
            slot_ = std::move(other.slot_);
        }
        return *this;
    }

    /** @copydoc PlatformRequestSubscription::Reset */
    void PlatformRequestSubscription::Reset() noexcept {
        if (slot_)
            slot_->Reset();
        slot_.reset();
    }

    /** @copydoc PlatformRequestSubscription::IsActive */
    bool PlatformRequestSubscription::IsActive() const noexcept {
        return slot_ && slot_->IsActive();
    }

    /** @copydoc PlatformRequestStore::PlatformRequestStore */
    PlatformRequestStore::PlatformRequestStore(PlatformRequestStoreConfig config) : state_(std::make_shared<State>(config)) {}

    /** @copydoc PlatformRequestStore::~PlatformRequestStore */
    PlatformRequestStore::~PlatformRequestStore() {
        Shutdown();
    }

    /** @brief Returns mutable request ownership state for state-changing frontend operations. */
    PlatformRequestStore::State &PlatformRequestStore::MutableState() noexcept {
        return *state_;
    }

    /** @copydoc PlatformRequestStore::Admit */
    Result<PlatformRequestId> PlatformRequestStore::AdmitErased(const std::type_index type) {
        auto &state = MutableState();
        const auto now = std::chrono::steady_clock::now();
        auto admitted = state.Admit(type, now);
        if (admitted.HasValue()) {
            Detail::RecordPlatformRequestQueueMetric(Detail::PlatformRequestQueueMetricOutcome::Accepted);
        } else {
            const Error &error = admitted.ErrorValue();
            if (ErrorChainContains(error, RequestErrors::CapacityExceeded.domain, RequestErrors::CapacityExceeded.code))
                Detail::RecordPlatformRequestQueueMetric(Detail::PlatformRequestQueueMetricOutcome::CapacityRejected);
            else if (ErrorChainContains(error, RequestErrors::FrontendUnavailable.domain, RequestErrors::FrontendUnavailable.code))
                Detail::RecordPlatformRequestQueueMetric(Detail::PlatformRequestQueueMetricOutcome::ShutdownRejected);
            else if (ErrorChainContains(error, RequestErrors::InvalidConfiguration.domain, RequestErrors::InvalidConfiguration.code))
                Detail::RecordPlatformRequestQueueMetric(Detail::PlatformRequestQueueMetricOutcome::InvalidConfiguration);
        }
        return admitted;
    }

    /** @copydoc PlatformRequestStore::MarkRunning */
    Result<PlatformRequestMutation> PlatformRequestStore::MarkRunningErased(const PlatformRequestId id,
                                                                            const PlatformRequestGeneration generation,
                                                                            const std::type_index type) {
        auto &state = MutableState();
        return state.MutateRecord(id, generation, type, [](State::Record &record) {
            using enum PlatformRequestState;
            auto &snapshot = record.snapshot;
            if (snapshot.state == Running)
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            if (snapshot.state != Queued)
                return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::InvalidTransition));
            snapshot.state = Running;
            snapshot.timing.startedAt = std::chrono::steady_clock::now();
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
        });
    }

    /** @copydoc PlatformRequestStore::RequestCancel */
    Result<PlatformRequestMutation> PlatformRequestStore::RequestCancelErased(const PlatformRequestId id,
                                                                              const PlatformRequestGeneration generation,
                                                                              const std::type_index type) {
        auto &state = MutableState();
        return state.MutateRecord(id, generation, type, State::CancelRecord);
    }

    /** @copydoc PlatformRequestStore::RecordObservationErased */
    Result<void> PlatformRequestStore::RecordObservationErased(const PlatformRequestId id, const PlatformRequestGeneration generation,
                                                               const std::type_index type, const Observation observation) {
        auto &state = MutableState();
        auto validated = [&]() {
            std::lock_guard lock(state.mutex);
            if (state.closed)
                return Result<void>::Failure(MakeError(RequestErrors::FrontendUnavailable));
            const auto *record = state.FindRecord(id, generation, type);
            if (record == nullptr)
                return Result<void>::Failure(MakeError(RequestErrors::Stale));
            if (IsTerminal(record->snapshot.state))
                return Result<void>::Failure(MakeError(RequestErrors::InvalidTransition));
            return Result<void>::Success();
        }();
        if (validated.HasError())
            return validated;
        if (observation == Observation::RetryScheduled)
            Detail::RecordPlatformRetryScheduledMetric();
        else if (observation == Observation::Throttled)
            Detail::RecordPlatformThrottledMetric();
        return validated;
    }

    /** @copydoc PlatformRequestStore::RequestCancel(PlatformRequestId, PlatformRequestGeneration) */
    Result<PlatformRequestMutation> PlatformRequestStore::RequestCancel(const PlatformRequestId id,
                                                                        const PlatformRequestGeneration generation) {
        auto &state = MutableState();
        return state.MutateRecordUntyped(id, generation, State::CancelRecord);
    }

    /** @copydoc PlatformRequestStore::CompleteSuccess */
    Result<PlatformRequestMutation> PlatformRequestStore::CompleteSuccess(const PlatformRequestHandle<void> &handle) {
        return CompleteErased(handle.Id(), handle.Generation(), typeid(void), PlatformRequestState::Succeeded, {}, std::nullopt);
    }

    /** @copydoc PlatformRequestStore::CompleteSuccess */
    Result<PlatformRequestMutation> PlatformRequestStore::CompleteSuccess(const PlatformRequestId id,
                                                                          const PlatformRequestGeneration generation) {
        return CompleteErased(id, generation, typeid(void), PlatformRequestState::Succeeded, {}, std::nullopt);
    }

    /** @copydoc PlatformRequestStore::CompleteFailure */
    Result<PlatformRequestMutation> PlatformRequestStore::CompleteErased(const PlatformRequestId id,
                                                                         const PlatformRequestGeneration generation,
                                                                         const std::type_index type,
                                                                         const PlatformRequestState terminalState,
                                                                         std::shared_ptr<const void> value, std::optional<Error> error) {
        auto &state = MutableState();
        auto completed = state.MutateRecord(id, generation, type,
                                            [&state, id, terminalState, value = std::move(value),
                                             error = std::move(error)](State::Record &record) mutable {
            if (IsTerminal(record.snapshot.state))
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            if (!State::CanComplete(record.snapshot.state, terminalState) || !State::TerminalShapeIsValid(terminalState, value, error))
                return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::InvalidTransition));

            record.snapshot.state = terminalState;
            record.snapshot.terminal = true;
            record.snapshot.value = std::move(value);
            record.snapshot.error = std::move(error);
            record.snapshot.timing.terminalAt = std::chrono::steady_clock::now();
            --state.activeCount;
            state.terminalOrder.push_back(id.value);
            state.QueueObservers(record);
            state.ExpireTerminalRecords();
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
        });
        if (completed.HasValue() && completed.Value() == PlatformRequestMutation::Applied)
            RecordTerminalMetric(terminalState);
        return completed;
    }

    /** @copydoc PlatformRequestStore::Query */
    Result<PlatformRequestStore::ErasedSnapshot> PlatformRequestStore::QueryErased(const PlatformRequestId id,
                                                                                   const PlatformRequestGeneration generation,
                                                                                   const std::type_index type) const {
        std::lock_guard lock(state_->mutex);
        if (!id.IsValid() || generation != state_->config.generation)
            return Result<ErasedSnapshot>::Failure(MakeError(RequestErrors::Stale));
        const auto found = state_->records.find(id.value);
        if (found == state_->records.end())
            return Result<ErasedSnapshot>::Failure(MakeError(RequestErrors::Expired));
        if (found->second.type != type)
            return Result<ErasedSnapshot>::Failure(MakeError(RequestErrors::Stale));
        return Result<ErasedSnapshot>::Success(found->second.snapshot);
    }

    /** @copydoc PlatformRequestStore::OnComplete */
    Result<PlatformRequestSubscription> PlatformRequestStore::SubscribeErased(const PlatformRequestId id,
                                                                              const PlatformRequestGeneration generation,
                                                                              const std::type_index type,
                                                                              std::function<void(const ErasedSnapshot &)> observer) {
        auto &state = MutableState();
        std::lock_guard lock(state.mutex);
        if (state.closed)
            return Result<PlatformRequestSubscription>::Failure(MakeError(RequestErrors::FrontendUnavailable));
        if (!id.IsValid() || generation != state.config.generation)
            return Result<PlatformRequestSubscription>::Failure(MakeError(RequestErrors::Stale));
        const auto found = state.records.find(id.value);
        if (found == state.records.end())
            return Result<PlatformRequestSubscription>::Failure(MakeError(RequestErrors::Expired));
        if (found->second.type != type)
            return Result<PlatformRequestSubscription>::Failure(MakeError(RequestErrors::Stale));
        if (!observer || state.observerCount >= state.config.observerCapacity)
            return Result<PlatformRequestSubscription>::Failure(MakeError(RequestErrors::CapacityExceeded));

        auto slot = std::make_shared<PlatformRequestSubscription::Slot>();
        const std::weak_ptr<State> weakState = state_;
        const auto *slotIdentity = slot.get();
        slot->observer = std::move(observer);
        slot->release = [weakState, id, slotIdentity]() noexcept {
            if (const auto state = weakState.lock()) {
                std::lock_guard stateLock(state->mutex);
                state->ReleaseObserver(id, slotIdentity);
            }
        };
        ++state.observerCount;
        if (found->second.snapshot.terminal)
            state.deliveries.push_back(State::Delivery{.slot = slot, .snapshot = found->second.snapshot, .context = found->second.context});
        else
            found->second.observers.emplace_back(slot);
        return Result<PlatformRequestSubscription>::Success(PlatformRequestSubscription{std::move(slot)});
    }

    /** @copydoc PlatformRequestStore::DispatchCompletions */
    std::size_t PlatformRequestStore::DispatchCompletions(const std::size_t maxCount) noexcept {
        auto &state = MutableState();
        if (!state.TryBeginDispatch(maxCount))
            return 0;

        const State::TurnGuard guard{state.dispatching};

        std::size_t delivered{};
        while (delivered < maxCount) {
            State::Delivery delivery;
            {
                std::lock_guard lock(state.mutex);
                if (state.deliveries.empty())
                    break;
                delivery = std::move(state.deliveries.front());
                state.deliveries.pop_front();
            }
            auto observer = delivery.slot->Take();
            if (!observer)
                continue;
            try {
                const Log::ScopedLogContext context(delivery.context);
                observer(delivery.snapshot);
            } catch (...) {
                std::lock_guard lock(state.mutex);
                ++state.callbackFailures;
            }
            ++delivered;
        }
        return delivered;
    }

    /** @copydoc PlatformRequestStore::Shutdown */
    void PlatformRequestStore::Shutdown() noexcept {
        if (!state_)
            return;
        auto &state = MutableState();
        std::vector<std::shared_ptr<PlatformRequestSubscription::Slot>> observers;
        std::deque<State::ProviderEvidence> retiredEvidence;
        std::size_t shutdownCount{};
        {
            std::lock_guard lock(state.mutex);
            if (state.closed)
                return;
            state.closed = true;
            const auto now = std::chrono::steady_clock::now();
            for (auto &[id, record] : state.records) {
                if (!IsTerminal(record.snapshot.state)) {
                    record.snapshot.state = PlatformRequestState::Failed;
                    record.snapshot.terminal = true;
                    record.snapshot.error = MakeError(RequestErrors::FrontendUnavailable);
                    record.snapshot.timing.terminalAt = now;
                    state.terminalOrder.push_back(id);
                    ++shutdownCount;
                }
                for (const auto &weak : record.observers)
                    if (auto slot = weak.lock())
                        observers.push_back(std::move(slot));
            }
            for (auto &delivery : state.deliveries)
                observers.push_back(std::move(delivery.slot));
            state.deliveries.clear();
            retiredEvidence.swap(state.evidence);
            state.activeCount = 0;
            state.ExpireTerminalRecords();
        }
        if (shutdownCount != 0)
            Detail::RecordPlatformRequestMetric(Detail::PlatformRequestMetricOutcome::Shutdown, static_cast<std::uint64_t>(shutdownCount));
        for (const auto &observer : observers)
            observer->Reset();
        // Explicitly retire payloads after unlocking and suppressing subscriptions; payload destruction may re-enter the store.
        retiredEvidence.clear();
    }

    /** @copydoc PlatformRequestStore::Generation */
    PlatformRequestGeneration PlatformRequestStore::Generation() const noexcept {
        return state_->config.generation;
    }

    /** @copydoc PlatformRequestStore::RecordCount */
    std::size_t PlatformRequestStore::RecordCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->records.size();
    }

    /** @copydoc PlatformRequestStore::ObserverCount */
    std::size_t PlatformRequestStore::ObserverCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->observerCount;
    }

    /** @copydoc PlatformRequestStore::CallbackFailureCount */
    std::uint64_t PlatformRequestStore::CallbackFailureCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->callbackFailures;
    }
}  // namespace Horo::PlatformServices
