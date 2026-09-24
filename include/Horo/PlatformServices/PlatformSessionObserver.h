#pragma once

/**
 * @file PlatformSessionObserver.h
 * @brief Bounded engine-thread dispatch for immutable generation-fenced session snapshots.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace Horo::PlatformServices {
    /** @brief Hard upper bound for session observers in one composed frontend. */
    inline constexpr std::size_t MaximumPlatformSessionObservers = 1'024;
    /** @brief Hard upper bound for provider notifications waiting for engine dispatch. */
    inline constexpr std::size_t MaximumPlatformSessionPendingNotifications = 4'096;

    /** @brief Stable failures emitted by the session observer/dispatch boundary. */
    namespace SessionObserverErrors {
        /** @brief Observer capacity or configuration exceeds the finite host bound. */
        extern const ErrorCodeDescriptor InvalidConfiguration;
        /** @brief A notification has no valid revision or immutable session evidence. */
        extern const ErrorCodeDescriptor InvalidNotification;
        /** @brief The observer has closed and cannot admit or dispatch new work. */
        extern const ErrorCodeDescriptor Closed;
        /** @brief The pending notification or observer capacity is exhausted. */
        extern const ErrorCodeDescriptor CapacityExceeded;
        /** @brief A notification is not newer than the already published session revision. */
        extern const ErrorCodeDescriptor StaleNotification;
        /** @brief Dispatch was attempted from a thread other than the composed engine owner. */
        extern const ErrorCodeDescriptor WrongThread;
        /** @brief Dispatch was recursively entered by an observer callback. */
        extern const ErrorCodeDescriptor ReentrantDispatch;
    }  // namespace SessionObserverErrors

    /** @brief Finite observer and pending-notification capacities for one session dispatcher. */
    struct PlatformSessionObserverConfig final {
        std::size_t maxObservers{64};
        std::size_t maxPendingNotifications{64};
    };

    /** @brief One immutable revisioned snapshot copied from a provider completion boundary. */
    struct PlatformSessionNotification final {
        std::uint64_t revision{};
        PlatformSessionSnapshot snapshot;
    };

    /** @brief Result of admitting a provider notification without invoking observers. */
    enum class PlatformSessionNotificationAdmission : std::uint8_t {
        Queued,
        IgnoredStale,
        IgnoredDuplicate
    };

    namespace SessionObserverDetail {
        struct State;
        struct Slot;
    }  // namespace SessionObserverDetail

    /** @brief Move-only RAII lifetime for one session observer registration. */
    class PlatformSessionObserverSubscription final {
    public:
        PlatformSessionObserverSubscription() = default;
        ~PlatformSessionObserverSubscription();
        PlatformSessionObserverSubscription(const PlatformSessionObserverSubscription &) = delete;
        PlatformSessionObserverSubscription &operator=(const PlatformSessionObserverSubscription &) = delete;
        PlatformSessionObserverSubscription(PlatformSessionObserverSubscription &&other) noexcept;
        PlatformSessionObserverSubscription &operator=(PlatformSessionObserverSubscription &&other) noexcept;

        /** @brief Deactivates the callback; a callback already executing may finish. */
        void Reset() noexcept;
        /** @brief Reports whether this token still owns an active registration. @return True while subscribed. */
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        friend class PlatformSessionObserver;
        PlatformSessionObserverSubscription(std::weak_ptr<SessionObserverDetail::State> state,
                                            std::shared_ptr<SessionObserverDetail::Slot> slot) noexcept;

        std::weak_ptr<SessionObserverDetail::State> state_;
        std::shared_ptr<SessionObserverDetail::Slot> slot_;
    };

    /**
     * @brief Bounded session notification owner that dispatches only on its composed engine thread.
     * @details Provider threads may enqueue copied immutable snapshots. Dispatch orders queued revisions, drops obsolete
     *          session generations, invokes callbacks outside the state lock, and never invokes a callback during admission.
     */
    class PlatformSessionObserver final {
    public:
        using Callback = std::function<void(const PlatformSessionNotification &)>;

        /**
         * @brief Creates one dispatcher bound to the calling engine thread.
         * @param config Positive finite observer and pending queue capacities.
         * @return Dispatcher or InvalidConfiguration.
         */
        [[nodiscard]] static Result<PlatformSessionObserver> Create(PlatformSessionObserverConfig config = {});

        PlatformSessionObserver() = delete;
        ~PlatformSessionObserver();
        PlatformSessionObserver(const PlatformSessionObserver &) = delete;
        PlatformSessionObserver &operator=(const PlatformSessionObserver &) = delete;
        PlatformSessionObserver(PlatformSessionObserver &&other) noexcept;
        PlatformSessionObserver &operator=(PlatformSessionObserver &&other) noexcept;

        /**
         * @brief Registers one deferred observer callback.
         * @param callback Callback invoked only by a successful owner-thread Dispatch.
         * @return Move-only subscription or closed/capacity/invalid failure.
         */
        [[nodiscard]] Result<PlatformSessionObserverSubscription> Subscribe(Callback callback);

        /**
         * @brief Queues one copied provider notification without running user code.
         * @param notification Revisioned immutable snapshot from the provider boundary.
         * @return Queued, ignored stale/duplicate admission, or typed lifecycle/capacity failure.
         */
        [[nodiscard]] Result<PlatformSessionNotificationAdmission> Enqueue(PlatformSessionNotification notification);

        /**
         * @brief Publishes all notifications present at entry on the composed engine thread.
         * @return Number of snapshots published, or WrongThread/ReentrantDispatch/Closed.
         * @post Provider threads and callbacks are never invoked by this method's admission counterpart.
         */
        [[nodiscard]] Result<std::size_t> Dispatch();

        /**
         * @brief Closes admission, revokes observers, and discards pending notifications idempotently.
         * @return Success; no callback is invoked by Close.
         */
        [[nodiscard]] Result<void> Close() noexcept;

        /** @brief Reports whether new subscriptions/notifications are rejected. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Returns pending notification count. @return Race-safe bounded count. */
        [[nodiscard]] std::size_t PendingCount() const noexcept;
        /** @brief Returns active callback count. @return Race-safe bounded count. */
        [[nodiscard]] std::size_t ObserverCount() const noexcept;
        /** @brief Returns the last published revision, or zero before first publication. */
        [[nodiscard]] std::uint64_t LastPublishedRevision() const noexcept;
        /** @brief Returns callback exceptions contained by the dispatch boundary. */
        [[nodiscard]] std::uint64_t CallbackFailureCount() const noexcept;

    private:
        explicit PlatformSessionObserver(std::shared_ptr<SessionObserverDetail::State> state) noexcept;
        std::shared_ptr<SessionObserverDetail::State> state_;
    };
}  // namespace Horo::PlatformServices
