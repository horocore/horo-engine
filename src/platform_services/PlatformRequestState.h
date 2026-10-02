#pragma once

/** @file PlatformRequestState.h
 * @brief Target-private bounded request, SDK ingress, and deferred observer ownership shared by the owner and ingress implementations.
 */

#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
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
            Log::LogContextSnapshot context;
            bool evidencePending{};
        };

        struct Delivery final {
            std::shared_ptr<PlatformRequestSubscription::Slot> slot;
            ErasedSnapshot snapshot;
            Log::LogContextSnapshot context;
        };

        struct ProviderEvidence final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
            std::type_index type{typeid(void)};
            PlatformRequestState terminalState{PlatformRequestState::Failed};
            std::shared_ptr<const void> value;
            std::optional<Error> error;
            Log::LogContextSnapshot context;
        };

        explicit State(const PlatformRequestStoreConfig &requested) : config(requested) {}

    private:
        friend class PlatformRequestStore;

        PlatformRequestStoreConfig config;
        mutable std::mutex mutex;
        std::unordered_map<std::uint64_t, Record> records;
        std::deque<std::uint64_t> terminalOrder;
        std::deque<Delivery> deliveries;
        // Protects copied SDK evidence and per-record deduplication with the same mutex as lifecycle state.
        // SDK threads only enqueue; the immutable ownerThread commits evidence and later invokes observers.
        std::deque<ProviderEvidence> evidence;
        const std::thread::id ownerThread{std::this_thread::get_id()};
        std::atomic_flag draining = ATOMIC_FLAG_INIT;
        std::uint64_t nextId{1};
        std::size_t activeCount{};
        std::size_t observerCount{};
        std::uint64_t callbackFailures{};
        bool closed{};
        std::atomic_flag dispatching = ATOMIC_FLAG_INIT;

        /** @brief Sole RAII owner of one dispatch/drain turn; copying could release another active turn. */
        class TurnGuard final {
        public:
            explicit TurnGuard(std::atomic_flag &flag) noexcept : flag_(flag) {}

            TurnGuard(const TurnGuard &) = delete;
            TurnGuard &operator=(const TurnGuard &) = delete;
            TurnGuard(TurnGuard &&) = delete;
            TurnGuard &operator=(TurnGuard &&) = delete;

            ~TurnGuard() {
                flag_.clear();
            }

        private:
            std::atomic_flag &flag_;
        };

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

        [[nodiscard]] Result<PlatformRequestId> Admit(const std::type_index type, const std::chrono::steady_clock::time_point now) {
            std::lock_guard lock(mutex);
            if (config.activeCapacity == 0 || config.terminalCapacity == 0 || config.observerCapacity == 0 ||
                !config.generation.IsValid() || config.completionCapacity == 0)
                return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::InvalidConfiguration));
            if (closed)
                return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::FrontendUnavailable));
            if (activeCount >= config.activeCapacity || nextId == 0)
                return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::CapacityExceeded));

            const PlatformRequestId id{nextId};
            Record record{.type = type,
                          .snapshot = ErasedSnapshot{.id = id,
                                                     .generation = config.generation,
                                                     .state = PlatformRequestState::Queued,
                                                     .timing = PlatformRequestTiming{.admittedAt = now}},
                          .context = Log::CaptureLogContext()};
            try {
                records.try_emplace(id.value, std::move(record));
            } catch (const std::bad_alloc &) {
                return Result<PlatformRequestId>::Failure(MakeError(RequestErrors::CapacityExceeded));
            }
            ++nextId;
            ++activeCount;
            return Result<PlatformRequestId>::Success(id);
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

        [[nodiscard]] Result<PlatformRequestMutation> Enqueue(ProviderEvidence completion);

        /** @brief Shares cancellation semantics for typed handles and provider-facing identity lookup. */
        [[nodiscard]] static Result<PlatformRequestMutation> CancelRecord(Record &record) {
            auto &snapshot = record.snapshot;
            if (IsTerminal(snapshot.state) || snapshot.cancellationRequested)
                return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
            snapshot.cancellationRequested = true;
            snapshot.timing.cancellationRequestedAt = std::chrono::steady_clock::now();
            snapshot.state = PlatformRequestState::Cancelling;
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
        }

        /** @brief Acquires owner-thread observer dispatch only outside provider drain and recursive callbacks. */
        [[nodiscard]] bool TryBeginDispatch(const std::size_t maxCount) noexcept {
            if (std::this_thread::get_id() != ownerThread || maxCount == 0 || draining.test())
                return false;
            return !dispatching.test_and_set();
        }

        [[nodiscard]] static bool CanComplete(const PlatformRequestState from, const PlatformRequestState to) noexcept {
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

        [[nodiscard]] static bool TerminalShapeIsValid(const PlatformRequestState state, const std::shared_ptr<const void> &value,
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

        void QueueObservers(Record &record) {
            for (const auto &weak : record.observers)
                if (auto slot = weak.lock())
                    deliveries.push_back(Delivery{.slot = std::move(slot), .snapshot = record.snapshot, .context = record.context});
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

}  // namespace Horo::PlatformServices
