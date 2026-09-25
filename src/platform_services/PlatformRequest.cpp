#include "Horo/PlatformServices/PlatformRequest.h"

#include "Horo/PlatformServices/PlatformRequestErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    struct PlatformRequestSubscription::Slot final {
        friend class PlatformRequestStore;

    private:
        mutable std::mutex mutex;
        bool active{true};
        std::function<void(const PlatformRequestStore::ErasedSnapshot &)> observer;
        std::function<void()> release;

    public:
        Slot() = default;

        void Reset() noexcept {
            std::function<void()> releaseOwner;
            {
                std::lock_guard lock(mutex);
                if (!active)
                    return;
                active = false;
                observer = {};
                releaseOwner = std::move(release);
            }
            if (releaseOwner)
                releaseOwner();
        }

        [[nodiscard]] bool IsActive() const noexcept {
            std::lock_guard lock(mutex);
            return active;
        }

        [[nodiscard]] std::function<void(const PlatformRequestStore::ErasedSnapshot &)> Take() noexcept {
            std::function<void(const PlatformRequestStore::ErasedSnapshot &)> callback;
            std::function<void()> releaseOwner;
            {
                std::lock_guard lock(mutex);
                if (!active)
                    return callback;
                active = false;
                callback = std::move(observer);
                releaseOwner = std::move(release);
            }
            if (releaseOwner)
                releaseOwner();
            return callback;
        }
    };

    struct PlatformRequestStore::State final {
        struct Record final {
            std::type_index type{typeid(void)};
            ErasedSnapshot snapshot;
            std::vector<std::weak_ptr<PlatformRequestSubscription::Slot>> observers;
        };

        struct Delivery final {
            std::shared_ptr<PlatformRequestSubscription::Slot> slot;
            ErasedSnapshot snapshot;
        };

        explicit State(const PlatformRequestStoreConfig &requested) : config(requested) {}

    private:
        friend class PlatformRequestStore;

        PlatformRequestStoreConfig config;
        mutable std::mutex mutex;
        std::unordered_map<std::uint64_t, Record> records;
        std::deque<std::uint64_t> terminalOrder;
        std::deque<Delivery> deliveries;
        std::uint64_t nextId{1};
        std::size_t activeCount{};
        std::size_t observerCount{};
        std::uint64_t callbackFailures{};
        bool closed{};
        std::atomic_flag dispatching = ATOMIC_FLAG_INIT;

        [[nodiscard]] Record *FindRecord(const PlatformRequestId id, const PlatformRequestGeneration generation,
                                         const std::type_index type) noexcept {
            auto *record = FindRecord(id, generation);
            return record != nullptr && record->type == type ? record : nullptr;
        }

        [[nodiscard]] Record *FindRecord(const PlatformRequestId id, const PlatformRequestGeneration generation) noexcept {
            if (!id.IsValid() || generation != config.generation)
                return nullptr;
            const auto found = records.find(id.value);
            if (found == records.end())
                return nullptr;
            return &found->second;
        }

        template <typename Operation>
        [[nodiscard]] Result<PlatformRequestMutation> MutateRecord(const PlatformRequestId id, const PlatformRequestGeneration generation,
                                                                   const std::type_index type, Operation &&operation) {
            std::lock_guard lock(mutex);
            auto *record = FindRecord(id, generation, type);
            if (record == nullptr)
                return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::Stale));
            return std::forward<Operation>(operation)(*record);
        }

        template <typename Operation>
        [[nodiscard]] Result<PlatformRequestMutation> MutateRecordUntyped(const PlatformRequestId id,
                                                                          const PlatformRequestGeneration generation,
                                                                          Operation &&operation) {
            std::lock_guard lock(mutex);
            auto *record = FindRecord(id, generation);
            if (record == nullptr)
                return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::Stale));
            return std::forward<Operation>(operation)(*record);
        }

        void QueueObservers(Record &record) {
            for (const auto &weak : record.observers)
                if (auto slot = weak.lock())
                    deliveries.push_back(Delivery{.slot = std::move(slot), .snapshot = record.snapshot});
            record.observers.clear();
        }

        void ExpireTerminalRecords() {
            while (terminalOrder.size() > config.terminalCapacity) {
                records.erase(terminalOrder.front());
                terminalOrder.pop_front();
            }
        }

        void ReleaseObserver(const PlatformRequestId id, const PlatformRequestSubscription::Slot *slotIdentity) {
            if (observerCount > 0)
                --observerCount;
            if (const auto record = records.find(id.value); record != records.end()) {
                std::erase_if(record->second.observers, [slotIdentity](const auto &weak) {
                    const auto candidate = weak.lock();
                    return !candidate || candidate.get() == slotIdentity;
                });
            }
            std::erase_if(deliveries, [slotIdentity](const auto &delivery) {
                return delivery.slot.get() == slotIdentity;
            });
        }
    };

    namespace {
        [[nodiscard]] bool CanComplete(const PlatformRequestState from, const PlatformRequestState to) noexcept {
            constexpr auto StateBit = [](const PlatformRequestState state) {
                return std::byte{static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(state))};
            };
            constexpr std::array<std::byte, 7> AllowedTerminalStates{
                StateBit(PlatformRequestState::Failed) | StateBit(PlatformRequestState::TimedOut),
                StateBit(PlatformRequestState::Succeeded) | StateBit(PlatformRequestState::Failed) |
                    StateBit(PlatformRequestState::TimedOut),
                StateBit(PlatformRequestState::Succeeded) | StateBit(PlatformRequestState::Failed) |
                    StateBit(PlatformRequestState::Cancelled) | StateBit(PlatformRequestState::TimedOut),
                std::byte{},
                std::byte{},
                std::byte{},
                std::byte{},
            };
            return (AllowedTerminalStates[static_cast<std::size_t>(from)] & StateBit(to)) != std::byte{};
        }

        [[nodiscard]] bool TerminalShapeIsValid(const PlatformRequestState state, const std::shared_ptr<const void> &value,
                                                const std::optional<Error> &error) noexcept {
            if (state == PlatformRequestState::Succeeded)
                return !error.has_value();
            if (!IsTerminal(state) || value || !error.has_value())
                return false;
            const auto &code = error->code.Value();
            const auto &domain = error->domain.Value();
            const auto isCancelled = code == RequestErrors::Cancelled.code.Value() && domain == RequestErrors::Cancelled.domain.Value();
            const auto isTimedOut = code == RequestErrors::TimedOut.code.Value() && domain == RequestErrors::TimedOut.domain.Value();
            if (state == PlatformRequestState::Cancelled)
                return isCancelled;
            if (state == PlatformRequestState::TimedOut)
                return isTimedOut;
            return !isCancelled && !isTimedOut;
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
        std::lock_guard lock(state.mutex);
        if (state.config.activeCapacity == 0 || state.config.terminalCapacity == 0 || state.config.observerCapacity == 0 ||
            !state.config.generation.IsValid())
            return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::InvalidConfiguration));
        if (state.closed)
            return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::FrontendUnavailable));
        if (state.activeCount >= state.config.activeCapacity)
            return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::CapacityExceeded));
        if (state.nextId == 0)
            return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::CapacityExceeded, "Request identity space is exhausted."));

        const PlatformRequestId id{state.nextId++};
        State::Record record{.type = type,
                             .snapshot = ErasedSnapshot{.id = id,
                                                        .generation = state.config.generation,
                                                        .state = PlatformRequestState::Queued,
                                                        .timing = PlatformRequestTiming{.admittedAt = now}}};
        state.records.try_emplace(id.value, std::move(record));
        ++state.activeCount;
        return Result<PlatformRequestId>::Success(id);
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
        return state.MutateRecord(id, generation, type, [](State::Record &record) {
            auto &snapshot = record.snapshot;
            if (IsTerminal(snapshot.state) || snapshot.cancellationRequested)
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            snapshot.cancellationRequested = true;
            snapshot.timing.cancellationRequestedAt = std::chrono::steady_clock::now();
            snapshot.state = PlatformRequestState::Cancelling;
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
        });
    }

    /** @copydoc PlatformRequestStore::RequestCancel(PlatformRequestId, PlatformRequestGeneration) */
    Result<PlatformRequestMutation> PlatformRequestStore::RequestCancel(const PlatformRequestId id,
                                                                        const PlatformRequestGeneration generation) {
        auto &state = MutableState();
        return state.MutateRecordUntyped(id, generation, [](State::Record &record) {
            auto &snapshot = record.snapshot;
            if (IsTerminal(snapshot.state) || snapshot.cancellationRequested)
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            snapshot.cancellationRequested = true;
            snapshot.timing.cancellationRequestedAt = std::chrono::steady_clock::now();
            snapshot.state = PlatformRequestState::Cancelling;
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
        });
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
        return state.MutateRecord(id, generation, type,
                                  [&state, id, terminalState, value = std::move(value),
                                   error = std::move(error)](State::Record &record) mutable {
            if (IsTerminal(record.snapshot.state))
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            if (!CanComplete(record.snapshot.state, terminalState) || !TerminalShapeIsValid(terminalState, value, error))
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
            state.deliveries.push_back(State::Delivery{.slot = slot, .snapshot = found->second.snapshot});
        else
            found->second.observers.emplace_back(slot);
        return Result<PlatformRequestSubscription>::Success(PlatformRequestSubscription{std::move(slot)});
    }

    /** @copydoc PlatformRequestStore::DispatchCompletions */
    std::size_t PlatformRequestStore::DispatchCompletions(const std::size_t maxCount) noexcept {
        if (maxCount == 0 || state_->dispatching.test_and_set())
            return 0;

        struct DispatchGuard final {
            std::atomic_flag &flag;

            explicit DispatchGuard(std::atomic_flag &flagToClear) noexcept : flag(flagToClear) {}

            DispatchGuard(const DispatchGuard &) = delete;
            DispatchGuard &operator=(const DispatchGuard &) = delete;
            DispatchGuard(DispatchGuard &&) = delete;
            DispatchGuard &operator=(DispatchGuard &&) = delete;

            ~DispatchGuard() {
                flag.clear();
            }
        };

        DispatchGuard guard{state_->dispatching};

        std::size_t delivered{};
        while (delivered < maxCount) {
            State::Delivery delivery;
            {
                std::lock_guard lock(state_->mutex);
                if (state_->deliveries.empty())
                    break;
                delivery = std::move(state_->deliveries.front());
                state_->deliveries.pop_front();
            }
            auto observer = delivery.slot->Take();
            if (!observer)
                continue;
            try {
                observer(delivery.snapshot);
            } catch (...) {
                std::lock_guard lock(state_->mutex);
                ++state_->callbackFailures;
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
                }
                for (const auto &weak : record.observers)
                    if (auto slot = weak.lock())
                        observers.push_back(std::move(slot));
            }
            for (auto &delivery : state.deliveries)
                observers.push_back(std::move(delivery.slot));
            state.deliveries.clear();
            state.activeCount = 0;
            state.ExpireTerminalRecords();
        }
        for (const auto &observer : observers)
            observer->Reset();
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
