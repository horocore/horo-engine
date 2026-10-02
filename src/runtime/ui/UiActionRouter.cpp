#include "Horo/Runtime/Ui/UiActions.h"
#include "Horo/Runtime/Ui/UiAsyncActions.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnownEnum(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
        }

        [[nodiscard]] const UiActionOwnerContext &InvalidOwnerContext() noexcept {
            static const UiActionOwnerContext invalid{};
            return invalid;
        }

        struct DispatchGuard final {
            explicit DispatchGuard(bool &dispatching) noexcept : dispatching_(dispatching), previous_(std::exchange(dispatching, true)) {}

            DispatchGuard(const DispatchGuard &) = delete;
            DispatchGuard &operator=(const DispatchGuard &) = delete;
            DispatchGuard(DispatchGuard &&) = delete;
            DispatchGuard &operator=(DispatchGuard &&) = delete;

            ~DispatchGuard() noexcept {
                dispatching_ = previous_;
            }

            bool &dispatching_;
            bool previous_;
        };

        /** @brief Contains foreign callback exceptions at the synchronous consumer boundary. */
        template <typename Callback> [[nodiscard]] auto InvokeActionHandler(Callback &&callback) {
            try {
                return callback();
            } catch (...) {  // NOSONAR(cpp:S2738) The Runtime UI callback boundary must contain arbitrary handler exceptions.
                return decltype(callback())::Failure(MakeError(UiErrors::ActionHandlerFailed));
            }
        }

    }  // namespace

    struct UiActionRouter::Storage final {
        Storage(const UiActionRouterDescriptor &descriptor, UiAsyncActionStore operations)
            : owner(descriptor.owner), queue(descriptor.maximumQueuedCommands), asyncActions(std::move(operations)) {}

        UiActionOwnerContext owner;
        std::vector<UiActionRequest> queue;
        UiAsyncActionStore asyncActions;
        std::size_t head{};
        std::size_t count{};
        std::uint64_t nextSequence{1};
        UiActionRouterState state{UiActionRouterState::Active};
        bool dispatching{};
    };

    /** @copydoc UiActionRouterDescriptor::IsValid */
    bool UiActionRouterDescriptor::IsValid() const noexcept {
        return owner.IsValid() && maximumQueuedCommands > 0 && maximumQueuedCommands <= MaximumUiActionCommands;
    }

    /** @copydoc UiActionRouter::Create */
    Result<UiActionRouter> UiActionRouter::Create(const UiActionRouterDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiActionRouter>(UiErrors::ActionInvalid);
        try {
            auto operations = UiAsyncActionStore::Create(descriptor.owner, descriptor.maximumQueuedCommands);
            if (operations.HasError())
                return Result<UiActionRouter>::Failure(operations.ErrorValue());
            return Result<UiActionRouter>::Success(UiActionRouter{std::make_shared<Storage>(descriptor, std::move(operations).Value())});
        } catch (const std::bad_alloc &) {
            return Failure<UiActionRouter>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiActionRouter::UiActionRouter */
    UiActionRouter::UiActionRouter(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiActionRouter::~UiActionRouter */
    UiActionRouter::~UiActionRouter() {
        Shutdown();
    }

    /** @copydoc UiActionRouter::UiActionRouter */
    UiActionRouter::UiActionRouter(UiActionRouter &&) noexcept = default;

    /** @copydoc UiActionRouter::operator= */
    UiActionRouter &UiActionRouter::operator=(UiActionRouter &&other) noexcept {
        if (this != &other) {
            if (storage_)
                storage_->asyncActions.Retire(UiActionCancellationReason::Superseded);
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiActionRouter::Enqueue */
    Result<UiActionRequestId> UiActionRouter::Enqueue(UiActionSource source, UiActionCommand command) {
        if (!storage_ || storage_->state != UiActionRouterState::Active || storage_->dispatching)
            return Failure<UiActionRequestId>(UiErrors::ActionLifecycleUnavailable);
        if (!source.IsValid())
            return Failure<UiActionRequestId>(UiErrors::ActionInvalid);
        if (source.owner != storage_->owner)
            return Failure<UiActionRequestId>(UiErrors::ActionSourceStale);
        if (const auto valid = ValidateUiActionCommand(command); valid.HasError())
            return Result<UiActionRequestId>::Failure(valid.ErrorValue());
        if (storage_->count == storage_->queue.size())
            return Failure<UiActionRequestId>(UiErrors::ActionQueueCapacityExceeded);
        if (storage_->nextSequence == 0)
            return Failure<UiActionRequestId>(UiErrors::GenerationExhausted);

        const auto sequence = UiActionSequence::Create(storage_->nextSequence);
        if (sequence.HasError())
            return Result<UiActionRequestId>::Failure(sequence.ErrorValue());
        if (storage_->nextSequence == std::numeric_limits<std::uint64_t>::max())
            storage_->nextSequence = 0;
        else
            ++storage_->nextSequence;

        const UiActionRequestId id{storage_->owner.instance.ownership, sequence.Value()};
        const std::size_t slot = (storage_->head + storage_->count) % storage_->queue.size();
        storage_->queue[slot] = UiActionRequest{id, std::move(source), UiActionOriginOf(command), std::move(command)};
        ++storage_->count;
        return Result<UiActionRequestId>::Success(id);
    }

    /** @copydoc UiActionRouter::TryDequeue */
    Result<std::optional<UiActionRequest>> UiActionRouter::TryDequeue() {
        if (!storage_ || storage_->state == UiActionRouterState::Stopped || storage_->dispatching)
            return Failure<std::optional<UiActionRequest>>(UiErrors::ActionLifecycleUnavailable);
        if (storage_->count == 0)
            return Result<std::optional<UiActionRequest>>::Success(std::nullopt);

        UiActionRequest request = std::move(storage_->queue[storage_->head]);
        storage_->queue[storage_->head] = UiActionRequest{};
        storage_->head = (storage_->head + 1) % storage_->queue.size();
        --storage_->count;
        return Result<std::optional<UiActionRequest>>::Success(std::optional<UiActionRequest>{std::move(request)});
    }

    /** @copydoc UiActionRouter::Dispatch */
    Result<UiActionResult> UiActionRouter::Dispatch(const UiActionRequest &request, UiActionHandler &handler) {
        if (!storage_ || storage_->state == UiActionRouterState::Stopped || storage_->dispatching)
            return Failure<UiActionResult>(UiErrors::ActionLifecycleUnavailable);
        if (const auto valid = request.Validate(); valid.HasError())
            return Result<UiActionResult>::Failure(valid.ErrorValue());
        if (request.source.owner != storage_->owner || request.id.ownership != storage_->owner.instance.ownership)
            return Failure<UiActionResult>(UiErrors::ActionSourceStale);

        const auto storage = storage_;
        DispatchGuard guard{storage->dispatching};
        auto result = InvokeActionHandler([&request, &handler] {
            return handler.Handle(request);
        });
        if (storage->state == UiActionRouterState::Stopped)
            return Failure<UiActionResult>(UiErrors::ActionLifecycleUnavailable);
        if (result.HasError())
            return Result<UiActionResult>::Failure(std::move(result).ErrorValue());
        if (result.Value().request != request.id)
            return Failure<UiActionResult>(UiErrors::ActionResultStale);
        if (const auto valid = result.Value().Validate(); valid.HasError())
            return Result<UiActionResult>::Failure(valid.ErrorValue());
        return Result<UiActionResult>::Success(std::move(result).Value());
    }

    /** @copydoc UiActionRouter::DispatchNext */
    Result<std::optional<UiActionResult>> UiActionRouter::DispatchNext(UiActionHandler &handler) {
        if (!storage_ || storage_->state == UiActionRouterState::Stopped || storage_->dispatching)
            return Failure<std::optional<UiActionResult>>(UiErrors::ActionLifecycleUnavailable);
        const auto request = TryDequeue();
        if (request.HasError())
            return Result<std::optional<UiActionResult>>::Failure(request.ErrorValue());
        if (!request.Value().has_value())
            return Result<std::optional<UiActionResult>>::Success(std::nullopt);

        auto result = Dispatch(*request.Value(), handler);
        if (result.HasError())
            return Result<std::optional<UiActionResult>>::Failure(result.ErrorValue());
        return Result<std::optional<UiActionResult>>::Success(std::optional<UiActionResult>{std::move(result).Value()});
    }

    /** @copydoc UiActionRouter::DispatchNext */
    Result<std::optional<UiActionResult>> UiActionRouter::DispatchNext(UiAsyncActionHandler &handler) {
        if (!storage_ || storage_->state != UiActionRouterState::Active || storage_->dispatching)
            return Failure<std::optional<UiActionResult>>(UiErrors::ActionLifecycleUnavailable);
        if (storage_->count == 0)
            return Result<std::optional<UiActionResult>>::Success(std::nullopt);
        const auto storage = storage_;
        const UiActionRequest request = storage->queue[storage->head];
        auto admitted = storage->asyncActions.Start(request);
        if (admitted.HasError())
            return Result<std::optional<UiActionResult>>::Failure(admitted.ErrorValue());
        auto producer = std::move(admitted).Value();
        const auto key = producer.Key();
        // Admission reserves state before removing the command; refusal never loses queued work.
        storage->queue[storage->head] = {};
        storage->head = (storage->head + 1) % storage->queue.size();
        --storage->count;
        DispatchGuard guard{storage->dispatching};
        if (auto scheduled = InvokeActionHandler([&request, &handler, &producer] {
            return handler.Start(request, std::move(producer));
        });
            scheduled.HasError()) {
            (void)storage->asyncActions.Cancel(key, UiActionCancellationReason::Requested);
            (void)storage->asyncActions.Release(key);
            return Result<std::optional<UiActionResult>>::Failure(std::move(scheduled).ErrorValue());
        }
        if (storage->state != UiActionRouterState::Active)
            return Failure<std::optional<UiActionResult>>(UiErrors::ActionLifecycleUnavailable);
        return Result<std::optional<UiActionResult>>::Success(UiActionResult::Pending(request.id, key.operation).Value());
    }

    /** @copydoc UiActionRouter::AsyncActions */
    UiAsyncActionStore *UiActionRouter::AsyncActions() noexcept {
        return storage_ ? &storage_->asyncActions : nullptr;
    }

    /** @copydoc UiActionRouter::Owner */
    const UiActionOwnerContext &UiActionRouter::Owner() const noexcept {
        return storage_ ? storage_->owner : InvalidOwnerContext();
    }

    /** @copydoc UiActionRouter::QueuedCount */
    std::size_t UiActionRouter::QueuedCount() const noexcept {
        return storage_ ? storage_->count : 0;
    }

    /** @copydoc UiActionRouter::BeginRetirement */
    Result<void> UiActionRouter::BeginRetirement(const UiActionCancellationReason reason) {
        if (!storage_ || storage_->state != UiActionRouterState::Active)
            return Failure(UiErrors::ActionLifecycleUnavailable);
        if (!IsKnownEnum(reason, UiActionCancellationReason::Count))
            return Failure(UiErrors::ActionResultInvalid);
        storage_->state = UiActionRouterState::Retiring;
        storage_->asyncActions.Retire(reason);
        return Result<void>::Success();
    }

    /** @copydoc UiActionRouter::Shutdown */
    void UiActionRouter::Shutdown() noexcept {
        if (!storage_)
            return;
        storage_->state = UiActionRouterState::Stopped;
        storage_->asyncActions.Retire(UiActionCancellationReason::Shutdown);
        storage_->head = 0;
        storage_->count = 0;
        storage_->queue.clear();
    }

    /** @copydoc UiActionRouter::State */
    UiActionRouterState UiActionRouter::State() const noexcept {
        return storage_ ? storage_->state : UiActionRouterState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
