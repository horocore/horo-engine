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
        /** @brief Preserves typed Runtime UI failure evidence at the router boundary. */
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Validates closed cancellation enums before mutating asynchronous lifecycle state. */
        template <typename Enum> [[nodiscard]] bool IsKnownEnum(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
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

        /** @brief Contains foreign callback exceptions at the synchronous consumer boundary. */
        template <typename Callback> [[nodiscard]] auto InvokeActionHandler(Callback &&callback) {
            try {
                return callback();
            } catch (...) {  // NOSONAR(cpp:S2738) The Runtime UI callback boundary must contain arbitrary handler exceptions.
                return decltype(callback())::Failure(MakeError(UiErrors::ActionHandlerFailed));
            }
        }

    }  // namespace

    /** @brief Owns the finite queue, asynchronous operations and ownership-generation sequence evidence. */
    struct UiActionRouter::Storage final {
        Storage(const UiActionRouterDescriptor &descriptor, UiAsyncActionStore operations)
            : owner(descriptor.owner), queue(descriptor.maximumQueuedCommands), asyncActions(std::move(operations)),
              nextSequence(descriptor.previousSequence.Value() + 1), lastIssued(descriptor.previousSequence) {}

        UiActionOwnerContext owner;
        std::vector<UiActionRequest> queue;
        UiAsyncActionStore asyncActions;
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

    /** @copydoc UiActionRouter::StateStorage */
    UiActionRouter::Storage *UiActionRouter::StateStorage() noexcept {
        return storage_.get();
    }

    /** @copydoc UiActionRouter::StateStorage */
    const UiActionRouter::Storage *UiActionRouter::StateStorage() const noexcept {
        return storage_.get();
    }

    /** @copydoc UiActionRouter::~UiActionRouter */
    UiActionRouter::~UiActionRouter() {
        Shutdown();
    }

    /** @copydoc UiActionRouter::UiActionRouter */
    UiActionRouter::UiActionRouter(UiActionRouter &&) noexcept = default;

    /** @copydoc UiActionRouter::operator= */
    UiActionRouter &UiActionRouter::operator=(UiActionRouter &&other) noexcept {
        if (this != &other) {
            if (auto *const storage = StateStorage())
                storage->asyncActions.Retire(UiActionCancellationReason::Superseded);
            Shutdown();
            storage_ = std::move(other.storage_);
            replacement_ = std::move(other.replacement_);
            replacementPrepared_ = std::exchange(other.replacementPrepared_, false);
            replacementSwap_ = std::exchange(other.replacementSwap_, false);
            replacementCount_ = other.replacementCount_;
            replacementSource_ = other.replacementSource_;
            replacementSequence_ = other.replacementSequence_;
        }
        return *this;
    }

    /** @copydoc UiActionRouter::Enqueue */
    Result<UiActionRequestId> UiActionRouter::Enqueue(UiActionSource source, UiActionCommand command) {
        auto *const storage = StateStorage();
        if (!storage || storage->state != UiActionRouterState::Active || storage->dispatching)
            return Failure<UiActionRequestId>(UiErrors::ActionLifecycleUnavailable);
        if (!source.IsValid())
            return Failure<UiActionRequestId>(UiErrors::ActionInvalid);
        if (source.owner != storage->owner)
            return Failure<UiActionRequestId>(UiErrors::ActionSourceStale);
        if (const auto valid = ValidateUiActionCommand(command); valid.HasError())
            return Result<UiActionRequestId>::Failure(valid.ErrorValue());
        if (storage->count == storage->queue.size())
            return Failure<UiActionRequestId>(UiErrors::ActionQueueCapacityExceeded);
        if (storage->nextSequence == 0)
            return Failure<UiActionRequestId>(UiErrors::GenerationExhausted);

        const auto sequence = UiActionSequence::Create(storage->nextSequence);
        if (sequence.HasError())
            return Result<UiActionRequestId>::Failure(sequence.ErrorValue());
        if (storage->nextSequence == std::numeric_limits<std::uint64_t>::max())
            storage->nextSequence = 0;
        else
            ++storage->nextSequence;

        const UiActionRequestId id{storage->owner.instance.ownership, sequence.Value()};
        storage->lastIssued = sequence.Value();
        const std::size_t slot = (storage->head + storage->count) % storage->queue.size();
        storage->queue[slot] = UiActionRequest{id, std::move(source), UiActionOriginOf(command), std::move(command)};
        ++storage->count;
        return Result<UiActionRequestId>::Success(id);
    }

    /** @copydoc UiActionRouter::TryDequeue */
    Result<std::optional<UiActionRequest>> UiActionRouter::TryDequeue() {
        auto *const storage = StateStorage();
        if (!storage || storage->state == UiActionRouterState::Stopped || storage->dispatching)
            return Failure<std::optional<UiActionRequest>>(UiErrors::ActionLifecycleUnavailable);
        if (storage->count == 0)
            return Result<std::optional<UiActionRequest>>::Success(std::nullopt);

        UiActionRequest request = std::move(storage->queue[storage->head]);
        storage->queue[storage->head] = UiActionRequest{};
        storage->head = (storage->head + 1) % storage->queue.size();
        --storage->count;
        return Result<std::optional<UiActionRequest>>::Success(std::optional<UiActionRequest>{std::move(request)});
    }

    /** @copydoc UiActionRouter::Dispatch */
    Result<UiActionResult> UiActionRouter::Dispatch(const UiActionRequest &request, UiActionHandler &handler) {
        auto *const storage = StateStorage();
        if (!storage || storage->state == UiActionRouterState::Stopped || storage->dispatching)
            return Failure<UiActionResult>(UiErrors::ActionLifecycleUnavailable);
        if (const auto valid = request.Validate(); valid.HasError())
            return Result<UiActionResult>::Failure(valid.ErrorValue());
        if (request.source.owner != storage->owner || request.id.ownership != storage->owner.instance.ownership)
            return Failure<UiActionResult>(UiErrors::ActionSourceStale);

        const auto lifetime = storage_;
        DispatchGuard guard{storage->dispatching};
        auto result = InvokeActionHandler([&request, &handler] {
            return handler.Handle(request);
        });
        if (lifetime->state == UiActionRouterState::Stopped)
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
        if (const auto *const storage = StateStorage(); !storage || storage->state == UiActionRouterState::Stopped || storage->dispatching)
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
        auto *const storage = StateStorage();
        if (!storage || storage->state != UiActionRouterState::Active || storage->dispatching)
            return Failure<std::optional<UiActionResult>>(UiErrors::ActionLifecycleUnavailable);
        if (storage->count == 0)
            return Result<std::optional<UiActionResult>>::Success(std::nullopt);
        const auto lifetime = storage_;
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
        if (lifetime->state != UiActionRouterState::Active)
            return Failure<std::optional<UiActionResult>>(UiErrors::ActionLifecycleUnavailable);
        return Result<std::optional<UiActionResult>>::Success(UiActionResult::Pending(request.id, key.operation).Value());
    }

    /** @copydoc UiActionRouter::AsyncActions */
    UiAsyncActionStore *UiActionRouter::AsyncActions() noexcept {
        auto *const storage = StateStorage();
        return storage ? &storage->asyncActions : nullptr;
    }

    /** @copydoc UiActionRouter::Owner */
    const UiActionOwnerContext &UiActionRouter::Owner() const noexcept {
        auto *const storage = StateStorage();
        return storage ? storage->owner : InvalidOwnerContext();
    }

    /** @copydoc UiActionRouter::LastIssuedSequence */
    UiActionSequence UiActionRouter::LastIssuedSequence() const noexcept {
        const auto *const storage = StateStorage();
        return storage ? storage->lastIssued : UiActionSequence{};
    }

    /** @copydoc UiActionRouter::QueuedCount */
    std::size_t UiActionRouter::QueuedCount() const noexcept {
        const auto *const storage = StateStorage();
        return storage ? storage->count : 0;
    }

    /** @copydoc UiActionRouter::BeginRetirement */
    Result<void> UiActionRouter::BeginRetirement(const UiActionCancellationReason reason) {
        auto *const storage = StateStorage();
        if (!storage || storage->state != UiActionRouterState::Active)
            return Failure(UiErrors::ActionLifecycleUnavailable);
        if (!IsKnownEnum(reason, UiActionCancellationReason::Count))
            return Failure(UiErrors::ActionResultInvalid);
        storage->state = UiActionRouterState::Retiring;
        storage->asyncActions.Retire(reason);
        return Result<void>::Success();
    }

    /** @copydoc UiActionRouter::Shutdown */
    void UiActionRouter::Shutdown() noexcept {
        auto *const storage = StateStorage();
        if (!storage)
            return;
        storage->state = UiActionRouterState::Stopped;
        storage->asyncActions.Retire(UiActionCancellationReason::Shutdown);
        storage->head = 0;
        storage->count = 0;
        storage->queue.clear();
    }

    /** @copydoc UiActionRouter::State */
    UiActionRouterState UiActionRouter::State() const noexcept {
        const auto *const storage = StateStorage();
        return storage ? storage->state : UiActionRouterState::Stopped;
    }

    /** @copydoc UiActionRouter::ReserveInteractionReplacement */
    Result<void> UiActionRouter::ReserveInteractionReplacement() {
        const auto *const active = StateStorage();
        if (!active || active->state != UiActionRouterState::Active || active->dispatching || replacementPrepared_)
            return Failure(UiErrors::ActionLifecycleUnavailable);
        if (replacement_)
            return Result<void>::Success();
        try {
            auto operations = UiAsyncActionStore::Create(active->owner, static_cast<std::uint32_t>(active->queue.size()));
            if (operations.HasError())
                return Result<void>::Failure(operations.ErrorValue());
            replacement_ =
                std::make_shared<Storage>(UiActionRouterDescriptor{active->owner, static_cast<std::uint32_t>(active->queue.size()),
                                                                   active->lastIssued},
                                          std::move(operations).Value());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiActionRouter::PrepareInteractionReplacement */
    Result<void> UiActionRouter::PrepareInteractionReplacement(const UiActionOwnerContext &owner) {
        const auto *const active = StateStorage();
        if (!active || !replacement_ || replacementPrepared_ || active->state != UiActionRouterState::Active || active->dispatching)
            return Failure(UiErrors::ActionLifecycleUnavailable);
        const auto &prior = active->owner;
        if (!owner.IsValid() || owner.instance != prior.instance || owner.canvas != prior.canvas || owner.document != prior.document ||
            owner.documentRevision != prior.documentRevision || owner.treeRevision != prior.treeRevision ||
            owner.interaction.Compare(prior.interaction) == UiRevisionRelation::Older)
            return Failure(UiErrors::ActionSourceStale);
        const bool swap = prior != owner;
        if (swap && (active->count != 0 || !active->asyncActions.CanPrepareReplacementSource() ||
                     !replacement_->asyncActions.CanPrepareReplacementSource()))
            return Failure(UiErrors::ActionLifecycleUnavailable);
        if (swap && active->nextSequence == 0)
            return Failure(UiErrors::GenerationExhausted);
        replacementSource_ = prior;
        replacementSequence_ = active->lastIssued;
        replacementCount_ = active->count;
        replacementSwap_ = swap;
        if (!swap) {
            replacementPrepared_ = true;
            return Result<void>::Success();
        }
        replacement_->owner = owner;
        replacement_->head = 0;
        replacement_->count = 0;
        replacement_->lastIssued = active->lastIssued;
        replacement_->nextSequence = active->nextSequence;
        replacement_->state = UiActionRouterState::Active;
        replacement_->dispatching = false;
        replacement_->asyncActions.PrepareReplacementSource(owner, active->lastIssued.Value());
        replacementSource_ = prior;
        replacementSequence_ = active->lastIssued;
        replacementPrepared_ = true;
        return Result<void>::Success();
    }

    /** @copydoc UiActionRouter::CanPublishInteractionReplacement */
    Result<void> UiActionRouter::CanPublishInteractionReplacement(const UiActionOwnerContext &owner) const {
        const auto *const active = StateStorage();
        if (!active || !replacement_ || !replacementPrepared_ || active->owner != replacementSource_ ||
            active->lastIssued != replacementSequence_ || active->count != replacementCount_ || active->dispatching ||
            active->state != UiActionRouterState::Active || (replacementSwap_ ? replacement_->owner : active->owner) != owner)
            return Failure(UiErrors::ActionSourceStale);
        if (replacementSwap_ && (active->count != 0 || !active->asyncActions.CanPrepareReplacementSource()))
            return Failure(UiErrors::ActionSourceStale);
        return Result<void>::Success();
    }

    /** @copydoc UiActionRouter::PublishInteractionReplacement */
    void UiActionRouter::PublishInteractionReplacement() noexcept {
        if (replacementSwap_) {
            storage_.swap(replacement_);
            replacement_->state = UiActionRouterState::Retiring;
        }
        replacementPrepared_ = false;
        replacementSwap_ = false;
    }

    /** @copydoc UiActionRouter::AbandonInteractionReplacement */
    void UiActionRouter::AbandonInteractionReplacement() noexcept {
        replacementPrepared_ = false;
        replacementSwap_ = false;
    }

    /** @copydoc UiActionRouter::DrainInteractionReplacement */
    std::size_t UiActionRouter::DrainInteractionReplacement() noexcept {
        if (!storage_ || replacementPrepared_ || storage_->dispatching)
            return 0;
        auto count = storage_->asyncActions.DrainReplacementSource();
        if (replacement_)
            count += replacement_->asyncActions.DrainReplacementSource();
        return count;
    }

}  // namespace Horo::Runtime::Ui
