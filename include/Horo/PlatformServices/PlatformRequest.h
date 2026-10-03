#pragma once

/**
 * @file PlatformRequest.h
 * @brief Typed identities, retained terminal snapshots, and bounded request-record ownership.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace Horo::PlatformServices {
    /** @brief Frontend-owned nonzero identity for one admitted platform request. */
    struct PlatformRequestId final {
        std::uint64_t value{}; /**< Zero is reserved for an invalid identity. */

        /** @brief Checks representation. @return Whether the identity is nonzero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformRequestId &) const noexcept = default;
    };

    /** @brief Nonzero generation fencing handles and observations to one frontend lifetime. */
    struct PlatformRequestGeneration final {
        std::uint64_t value{}; /**< Zero is reserved for an invalid generation. */

        /** @brief Checks representation. @return Whether the generation is nonzero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformRequestGeneration &) const noexcept = default;
    };

    /** @brief Exhaustive lifecycle of an admitted request. */
    enum class PlatformRequestState : std::uint8_t {
        Queued,
        Running,
        Cancelling,
        Succeeded,
        Failed,
        Cancelled,
        TimedOut,
    };

    /**
     * @brief Reports whether a request state is immutable and terminal.
     * @param state State to inspect.
     * @return True for succeeded, failed, cancelled, or timed-out requests.
     */
    [[nodiscard]] constexpr bool IsTerminal(const PlatformRequestState state) noexcept {
        return state == PlatformRequestState::Succeeded || state == PlatformRequestState::Failed ||
               state == PlatformRequestState::Cancelled || state == PlatformRequestState::TimedOut;
    }

    /** @brief Monotonic lifecycle timestamps copied into every request snapshot. */
    struct PlatformRequestTiming final {
        std::chrono::steady_clock::time_point admittedAt;
        std::optional<std::chrono::steady_clock::time_point> startedAt;
        std::optional<std::chrono::steady_clock::time_point> cancellationRequestedAt;
        std::optional<std::chrono::steady_clock::time_point> terminalAt;
    };

    /** @brief Immutable typed success value or typed failure retained by a terminal record. */
    template <typename T> class PlatformTerminalResult final {
    public:
        /** @brief Reports whether this terminal outcome is successful. */
        [[nodiscard]] bool HasValue() const noexcept {
            return static_cast<bool>(value_);
        }

        /** @brief Reports whether this terminal outcome is a typed failure. */
        [[nodiscard]] bool HasError() const noexcept {
            return error_.has_value();
        }

        /** @brief Returns the immutable successful payload. @return Null for a failure. */
        [[nodiscard]] const T *Value() const noexcept {
            return value_.get();
        }

        /** @brief Returns the immutable typed failure. @return Null for success. */
        [[nodiscard]] const Error *ErrorValue() const noexcept {
            return error_ ? &*error_ : nullptr;
        }

    private:
        template <typename> friend struct PlatformRequestSnapshot;
        friend class PlatformRequestStore;
        std::shared_ptr<const T> value_;
        std::optional<Error> error_;
    };

    /** @brief Void specialization retaining success or exactly one typed failure. */
    template <> class PlatformTerminalResult<void> final {
    public:
        /** @brief Reports whether this terminal outcome is successful. */
        [[nodiscard]] bool HasValue() const noexcept {
            return !error_.has_value();
        }

        /** @brief Reports whether this terminal outcome is a typed failure. */
        [[nodiscard]] bool HasError() const noexcept {
            return error_.has_value();
        }

        /** @brief Returns the immutable typed failure. @return Null for success. */
        [[nodiscard]] const Error *ErrorValue() const noexcept {
            return error_ ? &*error_ : nullptr;
        }

    private:
        template <typename> friend struct PlatformRequestSnapshot;
        friend class PlatformRequestStore;
        std::optional<Error> error_;
    };

    /** @brief Owned immutable observation of one admitted request. */
    template <typename T> struct PlatformRequestSnapshot final {
        PlatformRequestId id;
        PlatformRequestGeneration generation;
        PlatformRequestState state{PlatformRequestState::Queued};
        PlatformRequestTiming timing;
        bool cancellationRequested{};
        std::optional<PlatformTerminalResult<T>> terminal;
    };

    /**
     * @brief Move-only typed identity reference into a frontend-owned request store.
     * @details The handle owns no request/provider state. Dropping or moving it never cancels or erases work.
     */
    template <typename T> class PlatformRequestHandle final {
    public:
        PlatformRequestHandle() = default;
        PlatformRequestHandle(const PlatformRequestHandle &) = delete;
        PlatformRequestHandle &operator=(const PlatformRequestHandle &) = delete;

        PlatformRequestHandle(PlatformRequestHandle &&other) noexcept
            : id_(std::exchange(other.id_, {})), generation_(std::exchange(other.generation_, {})) {}

        PlatformRequestHandle &operator=(PlatformRequestHandle &&other) noexcept {
            if (this != &other) {
                id_ = std::exchange(other.id_, {});
                generation_ = std::exchange(other.generation_, {});
            }
            return *this;
        }

        /** @brief Returns the frontend-issued request identity. */
        [[nodiscard]] PlatformRequestId Id() const noexcept {
            return id_;
        }

        /** @brief Returns the frontend generation captured at admission. */
        [[nodiscard]] PlatformRequestGeneration Generation() const noexcept {
            return generation_;
        }

        /** @brief Reports whether this is a non-moved-from identity reference. */
        [[nodiscard]] bool IsValid() const noexcept {
            return id_.IsValid() && generation_.IsValid();
        }

    private:
        friend class PlatformRequestStore;
        friend class PlatformProviderLifecycleHost;

        PlatformRequestHandle(const PlatformRequestId id, const PlatformRequestGeneration generation) noexcept
            : id_(id), generation_(generation) {}

        PlatformRequestId id_;
        PlatformRequestGeneration generation_;
    };

    /** @brief Outcome of an idempotent request-state mutation. */
    enum class PlatformRequestMutation : std::uint8_t {
        Applied,
        Unchanged
    };

    /**
     * @brief Copyable, generation-fenced SDK ingress for one typed request.
     * @details Owns only a weak store lease. Provider threads enqueue owned immutable evidence, never request mutations or callbacks.
     *          Copies may publish concurrently. The first accepted evidence wins; capacity rejection leaves the sink retryable.
     *          Closing the store revokes every sink, including copies retained after store destruction.
     */
    template <typename T> class PlatformRequestCompletionSink final {
    public:
        PlatformRequestCompletionSink(const PlatformRequestCompletionSink &) = default;
        PlatformRequestCompletionSink &operator=(const PlatformRequestCompletionSink &) = default;

        /**
         * @brief Enqueues a copied/owned provider outcome without publishing terminal state.
         * @param outcome Normalized success or failure; cancellation acknowledgements use AcknowledgeCancellation.
         * @return Applied for accepted evidence, Unchanged for duplicate/terminal evidence, or typed ingress failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> Complete(Result<T> outcome) const {
            if (outcome.HasError())
                return enqueue_(PlatformRequestState::Failed, {}, std::move(outcome).ErrorValue());
            std::shared_ptr<const void> payload;
            if constexpr (!std::is_void_v<T>) {
                try {
                    payload = std::make_shared<const T>(std::move(outcome).Value());
                } catch (const std::bad_alloc &) {
                    return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::CapacityExceeded));
                }
            }
            return enqueue_(PlatformRequestState::Succeeded, std::move(payload), std::nullopt);
        }

        /**
         * @brief Enqueues acknowledged cancellation after cancellation intent was recorded.
         * @param error Canonical platform.request.cancelled error.
         * @return Accepted, duplicate, or typed invalid-transition/lifecycle failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> AcknowledgeCancellation(Error error) const {
            return enqueue_(PlatformRequestState::Cancelled, {}, std::move(error));
        }

    private:
        friend class PlatformRequestStore;
        using Enqueue =
            std::function<Result<PlatformRequestMutation>(PlatformRequestState, std::shared_ptr<const void>, std::optional<Error>)>;

        explicit PlatformRequestCompletionSink(Enqueue enqueue) : enqueue_(std::move(enqueue)) {}

        Enqueue enqueue_;
    };

    /** @brief Bounded provider-drain evidence; observer delivery is always a separate engine turn. */
    struct PlatformCompletionDrainReport final {
        std::size_t processed{}; /**< Evidence removed from the bounded queue, including late terminal evidence. */
        std::size_t applied{};   /**< Requests terminalized by this drain. */
        std::size_t discarded{}; /**< Evidence superseded by terminal publication or retention expiry. */
    };

    /** @brief Move-only observer lifetime; destruction suppresses any future callback without cancelling the request. */
    class PlatformRequestSubscription final {
    public:
        PlatformRequestSubscription() = default;
        ~PlatformRequestSubscription();
        PlatformRequestSubscription(const PlatformRequestSubscription &) = delete;
        PlatformRequestSubscription &operator=(const PlatformRequestSubscription &) = delete;
        PlatformRequestSubscription(PlatformRequestSubscription &&other) noexcept;
        PlatformRequestSubscription &operator=(PlatformRequestSubscription &&other) noexcept;

        /** @brief Suppresses future delivery and releases the retained callback. */
        void Reset() noexcept;
        /** @brief Reports whether this token can still suppress a pending delivery. */
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        friend class PlatformRequestStore;
        struct Slot;

        explicit PlatformRequestSubscription(std::shared_ptr<Slot> slot) noexcept : slot_(std::move(slot)) {}

        std::shared_ptr<Slot> slot_;
    };

    /** @brief Finite ownership and observation limits for one frontend request generation. */
    struct PlatformRequestStoreConfig final {
        std::size_t activeCapacity{256};
        std::size_t terminalCapacity{256};
        std::size_t observerCapacity{512};
        PlatformRequestGeneration generation{1};
        std::size_t completionCapacity{256}; /**< Finite SDK evidence ingress capacity. */
    };

    /**
     * @brief Thread-safe bounded owner of admitted request records and deferred observers.
     * @details Query, OnComplete, and RequestCancel may be called from any non-real-time thread. The frontend constructs the store on its
     * owner thread and calls Admit, MarkRunning, terminal completion, DispatchCompletions, and Shutdown on that lane; internal locking
     * makes races fail safely while preserving that affinity contract. Callbacks are never invoked by admission, mutation, cancellation, or
     * registration. The owner dispatches them later without store locks; recursive dispatch is suppressed and callback exceptions are
     * caught. Shutdown terminalizes every active record and suppresses queued observers. A callback already executing on the owner lane may
     * finish, so composition quiesces dispatch before destroying the store.
     */
    class PlatformRequestStore final {
    public:
        /**
         * @brief Creates one bounded frontend request generation.
         * @param config Nonzero capacities and generation validated before first admission.
         */
        explicit PlatformRequestStore(PlatformRequestStoreConfig config = {});
        ~PlatformRequestStore();
        PlatformRequestStore(const PlatformRequestStore &) = delete;
        PlatformRequestStore &operator=(const PlatformRequestStore &) = delete;
        PlatformRequestStore(PlatformRequestStore &&) = delete;
        PlatformRequestStore &operator=(PlatformRequestStore &&) = delete;

        /** @brief Atomically admits a typed queued request or returns a typed capacity/lifecycle failure. */
        template <typename T> [[nodiscard]] Result<PlatformRequestHandle<T>> Admit() {
            auto admitted = AdmitErased(typeid(T));
            if (admitted.HasError())
                return Result<PlatformRequestHandle<T>>::Failure(admitted.ErrorValue());
            return Result<PlatformRequestHandle<T>>::Success(PlatformRequestHandle<T>{admitted.Value(), Generation()});
        }

        /**
         * @brief Creates a provider ingress sink fenced to one current typed request and frontend/session generation.
         * @param handle Current admitted request identity; dropping it does not revoke the returned sink.
         * @return Weak-lifetime completion sink or typed stale/closed failure.
         * @details Session replacement must close the old store and compose a new generation before admitting work.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestCompletionSink<T>> CompletionSink(const PlatformRequestHandle<T> &handle) const {
            auto enqueue = CompletionSinkErased(handle.Id(), handle.Generation(), typeid(T));
            if (enqueue.HasError())
                return Result<PlatformRequestCompletionSink<T>>::Failure(enqueue.ErrorValue());
            return Result<PlatformRequestCompletionSink<T>>::Success(PlatformRequestCompletionSink<T>{std::move(enqueue).Value()});
        }

        /**
         * @brief Commits bounded SDK evidence on the thread that constructed the store, without invoking observers.
         * @param maxCount Maximum evidence records to process this turn; zero is a successful no-op.
         * @return Processing counts or typed wrong-thread/recursive/closed failure.
         * @details Observer callbacks run only during a later DispatchCompletions call. A callback cannot recursively drain ingress.
         */
        [[nodiscard]] Result<PlatformCompletionDrainReport> DrainProviderCompletions(std::size_t maxCount = 256);

        /** @brief Marks an admitted queued request running; repeated running publication is idempotent. */
        template <typename T> [[nodiscard]] Result<PlatformRequestMutation> MarkRunning(const PlatformRequestHandle<T> &handle) {
            return MarkRunningErased(handle.Id(), handle.Generation(), typeid(T));
        }

        /**
         * @brief Records cooperative cancellation from a provider completion boundary using copied request identity.
         * @param id Frontend-issued request identity.
         * @param generation Frontend generation captured when the request was admitted.
         * @return Applied, unchanged, or a typed stale/expired failure.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> RequestCancel(PlatformRequestId id, PlatformRequestGeneration generation) {
            return RequestCancelErased(id, generation, typeid(T));
        }

        /** @brief Publishes one immutable successful payload; later terminal publications are ignored. */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteSuccess(const PlatformRequestHandle<T> &handle, T value) {
            auto payload = std::make_shared<const T>(std::move(value));
            return CompleteErased(handle.Id(), handle.Generation(), typeid(T), PlatformRequestState::Succeeded,
                                  std::static_pointer_cast<const void>(std::move(payload)), std::nullopt);
        }

        /**
         * @brief Publishes a provider success by generation-fenced identity without requiring the move-only caller handle.
         * @param id Frontend-issued request identity.
         * @param generation Frontend generation captured when the request was admitted.
         * @param value Owned immutable provider result.
         * @return Applied, unchanged after an earlier terminal result, or a typed stale/expired failure.
         */
        template <typename T>
            requires(!std::is_void_v<T>)
        [[nodiscard]] Result<PlatformRequestMutation> CompleteSuccess(PlatformRequestId id, PlatformRequestGeneration generation, T value) {
            auto payload = std::make_shared<const T>(std::move(value));
            return CompleteErased(id, generation, typeid(T), PlatformRequestState::Succeeded,
                                  std::static_pointer_cast<const void>(std::move(payload)), std::nullopt);
        }

        /** @brief Publishes successful completion for a void request; later terminal publications are ignored. */
        [[nodiscard]] Result<PlatformRequestMutation> CompleteSuccess(const PlatformRequestHandle<void> &handle);

        /**
         * @brief Publishes provider success for a void request by generation-fenced identity.
         * @param id Frontend-issued request identity.
         * @param generation Frontend generation captured when the request was admitted.
         * @return Applied, unchanged after an earlier terminal result, or a typed stale/expired failure.
         */
        [[nodiscard]] Result<PlatformRequestMutation> CompleteSuccess(PlatformRequestId id, PlatformRequestGeneration generation);

        /** @brief Publishes one immutable typed failure; later terminal publications are ignored. */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteFailure(const PlatformRequestHandle<T> &handle, Error error) {
            return CompleteErased(handle.Id(), handle.Generation(), typeid(T), PlatformRequestState::Failed, {}, std::move(error));
        }

        /**
         * @brief Publishes provider failure by generation-fenced identity.
         * @param id Frontend-issued request identity.
         * @param generation Frontend generation captured when the request was admitted.
         * @param error Typed provider failure.
         * @return Applied, unchanged after an earlier terminal result, or a typed stale/expired failure.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteFailure(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                      Error error) {
            return CompleteErased(id, generation, typeid(T), PlatformRequestState::Failed, {}, std::move(error));
        }

        /** @brief Publishes acknowledged cancellation with a typed cancellation error. */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteCancelled(const PlatformRequestHandle<T> &handle, Error error) {
            return CompleteErased(handle.Id(), handle.Generation(), typeid(T), PlatformRequestState::Cancelled, {}, std::move(error));
        }

        /**
         * @brief Publishes acknowledged cancellation by generation-fenced identity.
         * @param id Frontend-issued request identity.
         * @param generation Frontend generation captured when the request was admitted.
         * @param error Typed cancellation failure.
         * @return Applied, unchanged after an earlier terminal result, or a typed stale/expired failure.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteCancelled(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                        Error error) {
            return CompleteErased(id, generation, typeid(T), PlatformRequestState::Cancelled, {}, std::move(error));
        }

        /** @brief Publishes frontend-owned timeout with a typed timeout error supplied by the later policy layer. */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteTimedOut(const PlatformRequestHandle<T> &handle, Error error) {
            return CompleteErased(handle.Id(), handle.Generation(), typeid(T), PlatformRequestState::TimedOut, {}, std::move(error));
        }

        /**
         * @brief Publishes frontend-owned timeout by generation-fenced identity.
         * @param id Frontend-issued request identity.
         * @param generation Frontend generation captured when the request was admitted.
         * @param error Typed timeout failure.
         * @return Applied, unchanged after an earlier terminal result, or a typed stale/expired failure.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestMutation> CompleteTimedOut(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                       Error error) {
            return CompleteErased(id, generation, typeid(T), PlatformRequestState::TimedOut, {}, std::move(error));
        }

        /** @brief Records cooperative cancellation once; repeated or post-terminal requests are successful no-ops. */
        template <typename T> [[nodiscard]] Result<PlatformRequestMutation> RequestCancel(const PlatformRequestHandle<T> &handle) {
            return RequestCancelErased(handle.Id(), handle.Generation(), typeid(T));
        }

        /**
         * @brief Records that the owning retry policy scheduled another attempt for an admitted request.
         * @param handle Current typed request handle.
         * @return Success when the request is current and nonterminal, or a typed stale/lifecycle failure.
         * @note This publishes one bounded counter only; it does not schedule or authorize a retry.
         */
        template <typename T> [[nodiscard]] Result<void> RecordRetryScheduled(const PlatformRequestHandle<T> &handle) {
            return RecordObservationErased(handle.Id(), handle.Generation(), typeid(T), Observation::RetryScheduled);
        }

        /**
         * @brief Records normalized provider throttling evidence for an admitted request.
         * @param handle Current typed request handle.
         * @return Success when the request is current and nonterminal, or a typed stale/lifecycle failure.
         * @note No provider text, retry-after value, account identity, or payload is copied into telemetry.
         */
        template <typename T> [[nodiscard]] Result<void> RecordThrottled(const PlatformRequestHandle<T> &handle) {
            return RecordObservationErased(handle.Id(), handle.Generation(), typeid(T), Observation::Throttled);
        }

        /**
         * @brief Records cooperative cancellation by untyped identity at a backend boundary.
         * @param id Frontend-issued request identity.
         * @param generation Request-store generation captured at admission.
         * @return Applied/unchanged cancellation intent or a typed stale failure.
         * @details Provider adapters use this overload when their ABI receives only the Horo request identity and generation.
         */
        [[nodiscard]] Result<PlatformRequestMutation> RequestCancel(PlatformRequestId id, PlatformRequestGeneration generation);

        /** @brief Returns the current owned snapshot or a typed stale/expired failure. */
        template <typename T> [[nodiscard]] Result<PlatformRequestSnapshot<T>> Query(const PlatformRequestHandle<T> &handle) const {
            auto queried = QueryErased(handle.Id(), handle.Generation(), typeid(T));
            if (queried.HasError())
                return Result<PlatformRequestSnapshot<T>>::Failure(queried.ErrorValue());
            return Result<PlatformRequestSnapshot<T>>::Success(ToTypedSnapshot<T>(queried.Value()));
        }

        /**
         * @brief Registers one at-most-once deferred terminal observer.
         * @param handle Current typed request handle.
         * @param observer Callback invoked only by a later non-recursive DispatchCompletions turn.
         * @return Move-only suppression token or a typed stale/expired/capacity failure.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformRequestSubscription> OnComplete(const PlatformRequestHandle<T> &handle,
                                                                     std::function<void(const PlatformRequestSnapshot<T> &)> observer) {
            auto erasedObserver = [observer = std::move(observer)](const ErasedSnapshot &snapshot) {
                const auto typed = ToTypedSnapshot<T>(snapshot);
                observer(typed);
            };
            return SubscribeErased(handle.Id(), handle.Generation(), typeid(T), std::move(erasedObserver));
        }

        /** @brief Delivers up to maxCount queued observers outside locks; wrong-thread, recursive, or overlapping provider-drain dispatch
         * returns zero. */
        [[nodiscard]] std::size_t DispatchCompletions(std::size_t maxCount = std::numeric_limits<std::size_t>::max()) noexcept;
        /** @brief Terminalizes all active records, suppresses observers, and permanently closes admission. */
        void Shutdown() noexcept;
        /** @brief Returns the immutable frontend generation owned by this store. */
        [[nodiscard]] PlatformRequestGeneration Generation() const noexcept;
        /** @brief Returns the current number of active and retained terminal records. */
        [[nodiscard]] std::size_t RecordCount() const noexcept;
        /** @brief Returns the number of retained observer callbacks, including queued delivery. */
        [[nodiscard]] std::size_t ObserverCount() const noexcept;
        /** @brief Returns callback exceptions caught by the dispatch boundary. */
        [[nodiscard]] std::uint64_t CallbackFailureCount() const noexcept;

    private:
        friend struct PlatformRequestSubscription::Slot;

        enum class Observation : std::uint8_t {
            RetryScheduled,
            Throttled
        };

        struct ErasedSnapshot final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
            PlatformRequestState state{PlatformRequestState::Queued};
            PlatformRequestTiming timing;
            bool cancellationRequested{};
            bool terminal{};
            std::shared_ptr<const void> value;
            std::optional<Error> error;
        };
        struct State;

        [[nodiscard]] State &MutableState() noexcept;
        [[nodiscard]] Result<PlatformRequestCompletionSink<void>::Enqueue> CompletionSinkErased(PlatformRequestId id,
                                                                                                PlatformRequestGeneration generation,
                                                                                                std::type_index type) const;

        [[nodiscard]] Result<PlatformRequestId> AdmitErased(std::type_index type);
        [[nodiscard]] Result<PlatformRequestMutation> MarkRunningErased(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                        std::type_index type);
        [[nodiscard]] Result<PlatformRequestMutation> RequestCancelErased(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                          std::type_index type);
        /** @brief Validates a current nonterminal request before recording one bounded retry or throttle observation. */
        [[nodiscard]] Result<void> RecordObservationErased(PlatformRequestId id, PlatformRequestGeneration generation, std::type_index type,
                                                           Observation observation);
        [[nodiscard]] Result<PlatformRequestMutation> CompleteErased(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                     std::type_index type, PlatformRequestState terminalState,
                                                                     std::shared_ptr<const void> value, std::optional<Error> error);
        [[nodiscard]] Result<ErasedSnapshot> QueryErased(PlatformRequestId id, PlatformRequestGeneration generation,
                                                         std::type_index type) const;
        [[nodiscard]] Result<PlatformRequestSubscription> SubscribeErased(PlatformRequestId id, PlatformRequestGeneration generation,
                                                                          std::type_index type,
                                                                          std::function<void(const ErasedSnapshot &)> observer);

        template <typename T> [[nodiscard]] static PlatformRequestSnapshot<T> ToTypedSnapshot(const ErasedSnapshot &snapshot) {
            PlatformRequestSnapshot<T> typed{.id = snapshot.id,
                                             .generation = snapshot.generation,
                                             .state = snapshot.state,
                                             .timing = snapshot.timing,
                                             .cancellationRequested = snapshot.cancellationRequested};
            if (snapshot.terminal) {
                PlatformTerminalResult<T> terminal;
                if constexpr (!std::is_void_v<T>)
                    terminal.value_ = std::static_pointer_cast<const T>(snapshot.value);
                terminal.error_ = snapshot.error;
                typed.terminal = std::move(terminal);
            }
            return typed;
        }

        std::shared_ptr<State> state_;
    };
}  // namespace Horo::PlatformServices
