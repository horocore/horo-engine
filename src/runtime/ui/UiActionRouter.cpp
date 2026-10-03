#include "Horo/Runtime/Ui/UiActions.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Preserves typed Runtime UI failure evidence at the router boundary. */
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Supplies stable invalid ownership evidence after a router has relinquished its storage. */
        [[nodiscard]] const UiActionOwnerContext &InvalidOwnerContext() noexcept {
            static const UiActionOwnerContext invalid{};
            return invalid;
        }

        /** @brief Fences synchronous action callbacks and restores the previous dispatch state on every return path. */
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

        /** @brief Contains arbitrary handler exceptions at the explicit Runtime UI callback boundary. */
        [[nodiscard]] Result<UiActionResult> InvokeActionHandler(const UiActionRequest &request, UiActionHandler &handler) {
            try {
                return handler.Handle(request);
            } catch (...) {  // NOSONAR(cpp:S2738) The Runtime UI callback boundary must contain arbitrary handler exceptions.
                return Failure<UiActionResult>(UiErrors::ActionHandlerFailed);
            }
        }

    }  // namespace

    /** @brief Owns one finite command queue and its ownership-generation sequence evidence. */
    struct UiActionRouter::Storage final {
        explicit Storage(const UiActionRouterDescriptor &descriptor)
            : owner(descriptor.owner), queue(descriptor.maximumQueuedCommands), nextSequence(descriptor.previousSequence.Value() + 1),
              lastIssued(descriptor.previousSequence) {}

        UiActionOwnerContext owner;
        std::vector<UiActionRequest> queue;
        std::size_t head{};
        std::size_t count{};
        std::uint64_t nextSequence{1};
        UiActionSequence lastIssued;
        UiActionRouterState state{UiActionRouterState::Active};
        bool dispatching{};
    };

    /** @copydoc UiActionRouterDescriptor::IsValid */
    bool UiActionRouterDescriptor::IsValid() const noexcept {
        return owner.IsValid() && maximumQueuedCommands > 0 && maximumQueuedCommands <= MaximumUiActionCommands &&
               previousSequence.Value() != std::numeric_limits<std::uint64_t>::max();
    }

    /** @copydoc UiActionRouter::Create */
    Result<UiActionRouter> UiActionRouter::Create(const UiActionRouterDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiActionRouter>(UiErrors::ActionInvalid);
        try {
            return Result<UiActionRouter>::Success(UiActionRouter{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiActionRouter>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiActionRouter::UiActionRouter */
    UiActionRouter::UiActionRouter(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiActionRouter::~UiActionRouter */
    UiActionRouter::~UiActionRouter() {
        Shutdown();
    }

    /** @copydoc UiActionRouter::UiActionRouter */
    UiActionRouter::UiActionRouter(UiActionRouter &&) noexcept = default;

    /** @copydoc UiActionRouter::operator= */
    UiActionRouter &UiActionRouter::operator=(UiActionRouter &&) noexcept = default;

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
        storage_->lastIssued = sequence.Value();
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

        DispatchGuard guard{storage_->dispatching};
        auto result = InvokeActionHandler(request, handler);
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

    /** @copydoc UiActionRouter::Owner */
    const UiActionOwnerContext &UiActionRouter::Owner() const noexcept {
        return storage_ ? storage_->owner : InvalidOwnerContext();
    }

    /** @copydoc UiActionRouter::LastIssuedSequence */
    UiActionSequence UiActionRouter::LastIssuedSequence() const noexcept {
        return storage_ ? storage_->lastIssued : UiActionSequence{};
    }

    /** @copydoc UiActionRouter::QueuedCount */
    std::size_t UiActionRouter::QueuedCount() const noexcept {
        return storage_ ? storage_->count : 0;
    }

    /** @copydoc UiActionRouter::BeginRetirement */
    Result<void> UiActionRouter::BeginRetirement() {
        if (!storage_ || storage_->state != UiActionRouterState::Active || storage_->dispatching)
            return Failure(UiErrors::ActionLifecycleUnavailable);
        storage_->state = UiActionRouterState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiActionRouter::Shutdown */
    void UiActionRouter::Shutdown() noexcept {
        if (!storage_ || storage_->dispatching)
            return;
        storage_->state = UiActionRouterState::Stopped;
        storage_->head = 0;
        storage_->count = 0;
        storage_->queue.clear();
    }

    /** @copydoc UiActionRouter::State */
    UiActionRouterState UiActionRouter::State() const noexcept {
        return storage_ ? storage_->state : UiActionRouterState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
