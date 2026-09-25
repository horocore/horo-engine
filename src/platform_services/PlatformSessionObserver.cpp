#include "Horo/PlatformServices/PlatformSessionObserver.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    namespace {
        const ErrorDomainId Domain{"horo.platform.session_observer"};

        [[nodiscard]] bool IsKnown(const PlatformSessionPhase phase) noexcept {
            return phase <= PlatformSessionPhase::Failed;
        }

        [[nodiscard]] bool IsKnown(const PlatformSessionReason reason) noexcept {
            return reason <= PlatformSessionReason::Shutdown;
        }

        [[nodiscard]] bool IsKnown(const PlatformSessionAccessState state) noexcept {
            return state <= PlatformSessionAccessState::Revoked;
        }

        [[nodiscard]] bool IsValidNotification(const PlatformSessionNotification &notification) noexcept {
            if (notification.revision == 0 || !IsKnown(notification.snapshot.Phase()) || !IsKnown(notification.snapshot.Reason()) ||
                !notification.snapshot.Generation().IsValid() || !notification.snapshot.ProviderGeneration().IsValid() ||
                !notification.snapshot.AccessRevision().IsValid())
                return false;
            if (notification.snapshot.Phase() == PlatformSessionPhase::Active && !notification.snapshot.Subject())
                return false;
            if (notification.snapshot.Phase() != PlatformSessionPhase::Active && notification.snapshot.Subject())
                return false;
            return std::ranges::all_of(notification.snapshot.Capabilities().services, [](const PlatformSessionAccessState state) {
                return IsKnown(state);
            });
        }
    }  // namespace

    namespace SessionObserverErrors {
        const ErrorCodeDescriptor InvalidConfiguration{Domain,
                                                       ErrorCode{"platform.session.observer.invalid_configuration"},
                                                       ErrorSeverity::Error,
                                                       "The session observer configuration is invalid.",
                                                       "Use positive capacities within the finite observer bounds.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidNotification{Domain,
                                                      ErrorCode{"platform.session.observer.invalid_notification"},
                                                      ErrorSeverity::Error,
                                                      "The session notification is malformed.",
                                                      "Publish a validated immutable session snapshot with a nonzero revision.",
                                                      false,
                                                      false};
        const ErrorCodeDescriptor Closed{Domain,
                                         ErrorCode{"platform.session.observer.closed"},
                                         ErrorSeverity::Error,
                                         "The session observer is closed.",
                                         "Recompose a new observer for the next frontend generation.",
                                         false,
                                         false};
        const ErrorCodeDescriptor CapacityExceeded{Domain,
                                                   ErrorCode{"platform.session.observer.capacity_exceeded"},
                                                   ErrorSeverity::Error,
                                                   "The session observer capacity is exhausted.",
                                                   "Drain notifications or increase the bounded composition capacity.",
                                                   true,
                                                   false};
        const ErrorCodeDescriptor StaleNotification{Domain,
                                                    ErrorCode{"platform.session.observer.stale_notification"},
                                                    ErrorSeverity::Error,
                                                    "The session notification is older than the published generation or revision.",
                                                    "Discard it; it cannot publish into the current session.",
                                                    false,
                                                    false};
        const ErrorCodeDescriptor WrongThread{Domain,
                                              ErrorCode{"platform.session.observer.wrong_thread"},
                                              ErrorSeverity::Error,
                                              "Session observers must dispatch on their composed engine thread.",
                                              "Post Dispatch to the owning engine lane.",
                                              true,
                                              false};
        const ErrorCodeDescriptor ReentrantDispatch{Domain,
                                                    ErrorCode{"platform.session.observer.reentrant_dispatch"},
                                                    ErrorSeverity::Error,
                                                    "Session observer dispatch was entered recursively.",
                                                    "Return from the callback and dispatch again on a later engine turn.",
                                                    true,
                                                    false};
    }  // namespace SessionObserverErrors

    namespace SessionObserverDetail {
        struct Slot final {
            std::uint64_t id{};
            PlatformSessionObserver::Callback callback;
            std::atomic<bool> active{true};
            // The engine thread holds this through callback invocation. Reset/Close may take it from any thread,
            // making their return a fence against new callbacks. Recursive locking permits callback self-revocation;
            // Close releases the state mutex before taking it to avoid a lock-order cycle.
            std::recursive_mutex invocationMutex;
        };

        class State final {
        public:
            explicit State(const PlatformSessionObserverConfig value) : config(value) {}

        private:
            friend class Horo::PlatformServices::PlatformSessionObserver;
            friend class Horo::PlatformServices::PlatformSessionObserverSubscription;
            PlatformSessionObserverConfig config;
            std::thread::id ownerThread{std::this_thread::get_id()};
            std::map<std::uint64_t, PlatformSessionNotification> pending;
            std::vector<std::shared_ptr<Slot>> slots;
            std::uint64_t nextSlotId{1};
            std::uint64_t latestSessionGeneration{};
            std::uint64_t lastPublishedRevision{};
            std::uint64_t callbackFailures{};
            bool closed{};
            bool dispatching{};

            mutable std::mutex mutex;
        };
    }  // namespace SessionObserverDetail

    PlatformSessionObserverSubscription::PlatformSessionObserverSubscription(std::weak_ptr<SessionObserverDetail::State> state,
                                                                             std::shared_ptr<SessionObserverDetail::Slot> slot) noexcept
        : state_(std::move(state)), slot_(std::move(slot)) {}

    PlatformSessionObserverSubscription::~PlatformSessionObserverSubscription() {
        Reset();
    }

    PlatformSessionObserverSubscription::PlatformSessionObserverSubscription(PlatformSessionObserverSubscription &&other) noexcept
        : state_(std::move(other.state_)), slot_(std::move(other.slot_)) {}

    PlatformSessionObserverSubscription &PlatformSessionObserverSubscription::operator=(
        PlatformSessionObserverSubscription &&other) noexcept {
        if (this != &other) {
            Reset();
            state_ = std::move(other.state_);
            slot_ = std::move(other.slot_);
        }
        return *this;
    }

    void PlatformSessionObserverSubscription::Reset() noexcept {
        if (slot_ != nullptr) {
            std::lock_guard lock(slot_->invocationMutex);
            slot_->active.store(false);
        }
        if (const auto state = state_.lock()) {
            std::lock_guard lock(state->mutex);
            std::erase_if(state->slots, [](const std::shared_ptr<SessionObserverDetail::Slot> &slot) {
                return !slot->active.load();
            });
        }
        slot_.reset();
        state_.reset();
    }

    bool PlatformSessionObserverSubscription::IsActive() const noexcept {
        return slot_ != nullptr && slot_->active.load();
    }

    /** @copydoc PlatformSessionObserver::Create */
    Result<PlatformSessionObserver> PlatformSessionObserver::Create(const PlatformSessionObserverConfig config) {
        if (config.maxObservers == 0 || config.maxObservers > MaximumPlatformSessionObservers || config.maxPendingNotifications == 0 ||
            config.maxPendingNotifications > MaximumPlatformSessionPendingNotifications)
            return Result<PlatformSessionObserver>::Failure(MakeError(SessionObserverErrors::InvalidConfiguration));
        return Result<PlatformSessionObserver>::Success(PlatformSessionObserver{std::make_shared<SessionObserverDetail::State>(config)});
    }

    PlatformSessionObserver::PlatformSessionObserver(std::shared_ptr<SessionObserverDetail::State> state) noexcept
        : state_(std::move(state)) {}

    PlatformSessionObserver::~PlatformSessionObserver() {
        static_cast<void>(Close());
    }

    PlatformSessionObserver::PlatformSessionObserver(PlatformSessionObserver &&other) noexcept : state_(std::move(other.state_)) {}

    PlatformSessionObserver &PlatformSessionObserver::operator=(PlatformSessionObserver &&other) noexcept {
        if (this != &other) {
            static_cast<void>(Close());
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc PlatformSessionObserver::Subscribe */
    Result<PlatformSessionObserverSubscription> PlatformSessionObserver::Subscribe(  // NOSONAR(cpp:S5817) The owner facade mutates shared
                                                                                     // state.
        Callback callback) {
        if (!callback)
            return Result<PlatformSessionObserverSubscription>::Failure(MakeError(SessionObserverErrors::InvalidNotification));
        std::lock_guard lock(state_->mutex);
        if (state_->closed)
            return Result<PlatformSessionObserverSubscription>::Failure(MakeError(SessionObserverErrors::Closed));
        std::erase_if(state_->slots, [](const std::shared_ptr<SessionObserverDetail::Slot> &slot) {
            return !slot->active.load();
        });
        if (state_->slots.size() >= state_->config.maxObservers)
            return Result<PlatformSessionObserverSubscription>::Failure(MakeError(SessionObserverErrors::CapacityExceeded));
        auto slot = std::make_shared<SessionObserverDetail::Slot>();
        slot->id = state_->nextSlotId++;
        slot->callback = std::move(callback);
        state_->slots.push_back(slot);
        return Result<PlatformSessionObserverSubscription>::Success(PlatformSessionObserverSubscription{state_, std::move(slot)});
    }

    /** @copydoc PlatformSessionObserver::Enqueue */
    Result<PlatformSessionNotificationAdmission> PlatformSessionObserver::Enqueue(  // NOSONAR(cpp:S5817) The owner facade mutates shared
                                                                                    // state.
        PlatformSessionNotification notification) {
        if (!IsValidNotification(notification))
            return Result<PlatformSessionNotificationAdmission>::Failure(MakeError(SessionObserverErrors::InvalidNotification));

        std::lock_guard lock(state_->mutex);
        if (state_->closed)
            return Result<PlatformSessionNotificationAdmission>::Failure(MakeError(SessionObserverErrors::Closed));
        const auto generation = notification.snapshot.Generation().value;
        if (generation < state_->latestSessionGeneration || notification.revision <= state_->lastPublishedRevision)
            return Result<PlatformSessionNotificationAdmission>::Success(PlatformSessionNotificationAdmission::IgnoredStale);
        if (state_->pending.contains(notification.revision))
            return Result<PlatformSessionNotificationAdmission>::Success(PlatformSessionNotificationAdmission::IgnoredDuplicate);
        if (generation > state_->latestSessionGeneration) {
            state_->latestSessionGeneration = generation;
            std::erase_if(state_->pending, [generation](const auto &entry) {
                return entry.second.snapshot.Generation().value < generation;
            });
        }
        if (state_->pending.size() >= state_->config.maxPendingNotifications)
            return Result<PlatformSessionNotificationAdmission>::Failure(MakeError(SessionObserverErrors::CapacityExceeded));
        state_->pending.try_emplace(notification.revision, std::move(notification));
        return Result<PlatformSessionNotificationAdmission>::Success(PlatformSessionNotificationAdmission::Queued);
    }

    /** @copydoc PlatformSessionObserver::Dispatch */
    Result<std::size_t> PlatformSessionObserver::Dispatch() {  // NOSONAR(cpp:S5817) The owner facade mutates shared state.
        if (std::this_thread::get_id() != state_->ownerThread)
            return Result<std::size_t>::Failure(MakeError(SessionObserverErrors::WrongThread));

        std::map<std::uint64_t, PlatformSessionNotification> pending;
        std::vector<std::shared_ptr<SessionObserverDetail::Slot>> slots;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed)
                return Result<std::size_t>::Failure(MakeError(SessionObserverErrors::Closed));
            if (state_->dispatching)
                return Result<std::size_t>::Failure(MakeError(SessionObserverErrors::ReentrantDispatch));
            slots = state_->slots;
            state_->dispatching = true;
            pending.swap(state_->pending);
        }

        std::size_t published{};
        for (const auto &[revision, notification] : pending) {
            {
                std::lock_guard lock(state_->mutex);
                if (state_->closed)
                    break;
                if (revision <= state_->lastPublishedRevision || notification.snapshot.Generation().value < state_->latestSessionGeneration)
                    continue;
                state_->lastPublishedRevision = revision;
            }
            ++published;
            for (const auto &slot : slots) {
                std::lock_guard invocationLock(slot->invocationMutex);
                if (!slot->active.load())
                    continue;
                try {
                    slot->callback(notification);
                } catch (...) {  // NOSONAR(cpp:S1181): observer exceptions are contained at the engine boundary.
                    std::lock_guard lock(state_->mutex);
                    ++state_->callbackFailures;
                }
            }
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->dispatching = false;
        }
        return Result<std::size_t>::Success(published);
    }

    /** @copydoc PlatformSessionObserver::Close */
    Result<void> PlatformSessionObserver::Close() noexcept {  // NOSONAR(cpp:S5817) The owner facade mutates shared state.
        if (!state_)
            return Result<void>::Success();
        std::vector<std::shared_ptr<SessionObserverDetail::Slot>> slots;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed)
                return Result<void>::Success();
            state_->closed = true;
            state_->pending.clear();
            slots.swap(state_->slots);
        }
        for (const auto &slot : slots) {
            std::lock_guard invocationLock(slot->invocationMutex);
            slot->active.store(false);
        }
        return Result<void>::Success();
    }

    bool PlatformSessionObserver::IsClosed() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->closed;
    }

    std::size_t PlatformSessionObserver::PendingCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->pending.size();
    }

    std::size_t PlatformSessionObserver::ObserverCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return std::ranges::count_if(state_->slots, [](const std::shared_ptr<SessionObserverDetail::Slot> &slot) {
            return slot->active.load();
        });
    }

    std::uint64_t PlatformSessionObserver::LastPublishedRevision() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->lastPublishedRevision;
    }

    std::uint64_t PlatformSessionObserver::CallbackFailureCount() const noexcept {
        std::lock_guard lock(state_->mutex);
        return state_->callbackFailures;
    }
}  // namespace Horo::PlatformServices
