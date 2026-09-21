#include "Horo/Runtime/Ui/UiActions.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnownEnum(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
        }

        [[nodiscard]] bool IsValidActionValue(const UiActionValue &value) noexcept {
            return std::visit([]<typename Typed>(const Typed &typed) noexcept {
                using Value = std::decay_t<Typed>;
                if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(typed);
                else if constexpr (std::is_same_v<Value, UiActionText>)
                    return typed.IsValid() && IsValidUtf8ScalarSequence(typed.View());
                else if constexpr (std::is_same_v<Value, UiActionId> || std::is_same_v<Value, UiDocumentId> ||
                                   std::is_same_v<Value, UiCanvasId> || std::is_same_v<Value, UiElementId>)
                    return typed.IsValid();
                else
                    return true;
            }, value);
        }

        [[nodiscard]] Result<void> ValidateActionValue(const UiActionValue &value) {
            return IsValidActionValue(value) ? Result<void>::Success() : Failure(UiErrors::ActionPayloadInvalid);
        }

        [[nodiscard]] bool SameOwner(const UiActionOwnerContext &owner, const UiElementHandle element) noexcept {
            return owner.instance.IsValid() && element.IsValid() && element.ownership == owner.instance.ownership;
        }

        [[nodiscard]] bool NavigationMatchesOwner(const UiDefaultNavigationResult &navigation,
                                                  const UiOwnershipGeneration ownership) noexcept {
            if (!ownership.IsValid() || !navigation.IsValid())
                return false;
            if (navigation.from.has_value() && navigation.from->ownership != ownership)
                return false;
            if (navigation.target.has_value() && navigation.target->ownership != ownership)
                return false;
            return true;
        }

        [[nodiscard]] Result<void> ValidateRequestId(const UiActionRequestId request) {
            return request.IsValid() ? Result<void>::Success() : Failure(UiErrors::ActionInvalid);
        }

        [[nodiscard]] Result<void> ValidateOperation(const UiActionOperationId operation, const UiActionRequestId request) {
            if (!operation.IsValid() || operation.ownership != request.ownership)
                return Failure(UiErrors::ActionResultInvalid);
            return Result<void>::Success();
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

        [[nodiscard]] Result<UiActionResult> InvokeActionHandler(const UiActionRequest &request, UiActionHandler &handler) {
            try {
                return handler.Handle(request);
            } catch (...) {  // NOSONAR(cpp:S2738) The Runtime UI callback boundary must contain arbitrary handler exceptions.
                return Failure<UiActionResult>(UiErrors::ActionHandlerFailed);
            }
        }

        template <typename Command> [[nodiscard]] Result<void> ValidateTypedUiActionCommand(const Command &command) {
            if constexpr (std::is_same_v<Command, UiButtonActionCommand>) {
                if (!command.action.IsValid())
                    return Failure(UiErrors::ActionCommandInvalid);
                return command.payload.Validate();
            } else if constexpr (std::is_same_v<Command, UiFormActionCommand>) {
                if (!command.action.IsValid() || !IsKnownEnum(command.kind, UiFormActionKind::Count))
                    return Failure(UiErrors::ActionCommandInvalid);
                return command.payload.Validate();
            } else if constexpr (std::is_same_v<Command, UiRouteActionCommand>) {
                if (!command.action.IsValid() || !IsKnownEnum(command.kind, UiRouteActionKind::Count))
                    return Failure(UiErrors::ActionCommandInvalid);
                return command.payload.Validate();
            } else if constexpr (std::is_same_v<Command, UiGameplayActionCommand>) {
                if (!command.action.IsValid())
                    return Failure(UiErrors::ActionCommandInvalid);
                return command.payload.Validate();
            } else {
                if (!command.focused.IsValid() || !IsKnownEnum(command.direction, UiNavigationDirection::Count))
                    return Failure(UiErrors::NavigationInvalid);
                return Result<void>::Success();
            }
        }

        struct UiActionCommandValidationVisitor final {
            template <typename Command> [[nodiscard]] Result<void> operator()(const Command &command) const {
                return ValidateTypedUiActionCommand(command);
            }
        };
    }  // namespace

    /** @copydoc UiActionText::Create */
    Result<UiActionText> UiActionText::Create(const std::string_view value) {
        if (value.size() > MaximumUiActionTextBytes || !IsValidUtf8ScalarSequence(value))
            return Failure<UiActionText>(UiErrors::ActionPayloadInvalid);

        UiActionText result;
        std::ranges::copy(value, result.bytes.begin());
        result.size = static_cast<std::uint16_t>(value.size());
        return Result<UiActionText>::Success(std::move(result));
    }

    /** @copydoc UiActionText::View */
    std::string_view UiActionText::View() const noexcept {
        return {bytes.data(), size};
    }

    /** @copydoc UiActionText::IsValid */
    bool UiActionText::IsValid() const noexcept {
        return size <= MaximumUiActionTextBytes;
    }

    /** @copydoc UiActionPayload::Create */
    Result<UiActionPayload> UiActionPayload::Create(const std::span<const UiActionValue> values) {
        if (values.size() > MaximumUiActionArguments)
            return Failure<UiActionPayload>(UiErrors::ActionPayloadCapacityExceeded);

        UiActionPayload result;
        for (const UiActionValue &value : values) {
            if (const auto added = result.Add(value); added.HasError())
                return Result<UiActionPayload>::Failure(added.ErrorValue());
        }
        return Result<UiActionPayload>::Success(std::move(result));
    }

    /** @copydoc UiActionPayload::Add */
    Result<void> UiActionPayload::Add(UiActionValue value) {
        if (count_ >= MaximumUiActionArguments)
            return Failure(UiErrors::ActionPayloadCapacityExceeded);
        if (const auto valid = ValidateActionValue(value); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        values_[count_++] = std::move(value);
        return Result<void>::Success();
    }

    /** @copydoc UiActionPayload::Values */
    std::span<const UiActionValue> UiActionPayload::Values() const noexcept {
        return {values_.data(), count_};
    }

    /** @copydoc UiActionPayload::Size */
    std::size_t UiActionPayload::Size() const noexcept {
        return count_;
    }

    /** @copydoc UiActionPayload::Validate */
    Result<void> UiActionPayload::Validate() const {
        if (count_ > MaximumUiActionArguments)
            return Failure(UiErrors::ActionPayloadCapacityExceeded);
        for (std::size_t index = 0; index < count_; ++index)
            if (const auto valid = ValidateActionValue(values_[index]); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc UiActionOwnerContext::IsValid */
    bool UiActionOwnerContext::IsValid() const noexcept {
        return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership && document.IsValid() &&
               documentRevision.IsValid() && treeRevision.IsValid() && interaction.IsValid();
    }

    /** @copydoc UiActionSource::IsValid */
    bool UiActionSource::IsValid() const noexcept {
        return owner.IsValid() && SameOwner(owner, element);
    }

    /** @copydoc UiActionRequestId::IsValid */
    bool UiActionRequestId::IsValid() const noexcept {
        return ownership.IsValid() && sequence.IsValid();
    }

    /** @copydoc UiActionOperationId::IsValid */
    bool UiActionOperationId::IsValid() const noexcept {
        return ownership.IsValid() && sequence.IsValid();
    }

    /** @copydoc UiActionCommandKindOf */
    UiActionCommandKind UiActionCommandKindOf(const UiActionCommand &command) noexcept {
        return std::visit([]<typename Typed>(const Typed &) noexcept {
            using Value = std::decay_t<Typed>;
            using enum UiActionCommandKind;
            if constexpr (std::is_same_v<Value, UiButtonActionCommand>)
                return Button;
            else if constexpr (std::is_same_v<Value, UiFormActionCommand>)
                return Form;
            else if constexpr (std::is_same_v<Value, UiRouteActionCommand>)
                return Route;
            else if constexpr (std::is_same_v<Value, UiGameplayActionCommand>)
                return Gameplay;
            else
                return Navigation;
        }, command);
    }

    /** @copydoc UiActionOriginOf */
    UiActionOrigin UiActionOriginOf(const UiActionCommand &command) noexcept {
        switch (UiActionCommandKindOf(command)) {
            case UiActionCommandKind::Button:
                return UiActionOrigin::Button;
            case UiActionCommandKind::Form:
                return UiActionOrigin::Form;
            case UiActionCommandKind::Route:
                return UiActionOrigin::Route;
            case UiActionCommandKind::Gameplay:
                return UiActionOrigin::Gameplay;
            case UiActionCommandKind::Navigation:
                return UiActionOrigin::Navigation;
            case UiActionCommandKind::Count:
                break;
        }
        return UiActionOrigin::Count;
    }

    /** @copydoc ValidateUiActionCommand */
    Result<void> ValidateUiActionCommand(const UiActionCommand &command) {
        return std::visit(UiActionCommandValidationVisitor{}, command);
    }

    /** @copydoc UiActionRequest::Validate */
    Result<void> UiActionRequest::Validate() const {
        if (const auto valid = ValidateRequestId(id); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (!source.IsValid())
            return Failure(UiErrors::ActionInvalid);
        if (id.ownership != source.owner.instance.ownership)
            return Failure(UiErrors::ActionSourceStale);
        if (!IsKnownEnum(origin, UiActionOrigin::Count) || origin != UiActionOriginOf(command))
            return Failure(UiErrors::ActionCommandInvalid);
        if (const auto valid = ValidateUiActionCommand(command); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (const auto *navigation = std::get_if<UiNavigationCommand>(&command);
            navigation != nullptr && navigation->focused.ownership != source.owner.instance.ownership)
            return Failure(UiErrors::NavigationInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiDefaultNavigationResult::FocusMoved */
    Result<UiDefaultNavigationResult> UiDefaultNavigationResult::FocusMoved(const UiElementHandle from, const UiElementHandle target) {
        if (!from.IsValid() || !target.IsValid() || from.ownership != target.ownership || from == target)
            return Failure<UiDefaultNavigationResult>(UiErrors::NavigationInvalid);
        return Result<UiDefaultNavigationResult>::Success(UiDefaultNavigationResult{UiDefaultNavigationOutcome::FocusMoved, from, target});
    }

    /** @copydoc UiDefaultNavigationResult::SubmitDispatched */
    Result<UiDefaultNavigationResult> UiDefaultNavigationResult::SubmitDispatched(const UiElementHandle target) {
        if (!target.IsValid())
            return Failure<UiDefaultNavigationResult>(UiErrors::NavigationInvalid);
        return Result<UiDefaultNavigationResult>::Success(
            UiDefaultNavigationResult{UiDefaultNavigationOutcome::SubmitDispatched, std::nullopt, target});
    }

    /** @copydoc UiDefaultNavigationResult::CancelDispatched */
    Result<UiDefaultNavigationResult> UiDefaultNavigationResult::CancelDispatched(const UiElementHandle target) {
        if (!target.IsValid())
            return Failure<UiDefaultNavigationResult>(UiErrors::NavigationInvalid);
        return Result<UiDefaultNavigationResult>::Success(
            UiDefaultNavigationResult{UiDefaultNavigationOutcome::CancelDispatched, std::nullopt, target});
    }

    /** @copydoc UiDefaultNavigationResult::NoTarget */
    Result<UiDefaultNavigationResult> UiDefaultNavigationResult::NoTarget(const UiElementHandle from) {
        if (!from.IsValid())
            return Failure<UiDefaultNavigationResult>(UiErrors::NavigationInvalid);
        return Result<UiDefaultNavigationResult>::Success(
            UiDefaultNavigationResult{UiDefaultNavigationOutcome::NoTarget, from, std::nullopt});
    }

    /** @copydoc UiDefaultNavigationResult::IsValid */
    bool UiDefaultNavigationResult::IsValid() const noexcept {
        switch (outcome) {
            case UiDefaultNavigationOutcome::FocusMoved:
                return from.has_value() && target.has_value() && from->IsValid() && target->IsValid() && *from != *target;
            case UiDefaultNavigationOutcome::SubmitDispatched:
            case UiDefaultNavigationOutcome::CancelDispatched:
                return !from.has_value() && target.has_value() && target->IsValid();
            case UiDefaultNavigationOutcome::NoTarget:
                return from.has_value() && from->IsValid() && !target.has_value();
            case UiDefaultNavigationOutcome::Count:
                break;
        }
        return false;
    }

    /** @copydoc UiActionResult::Handled */
    Result<UiActionResult> UiActionResult::Handled(const UiActionRequestId request) {
        if (!request.IsValid())
            return Failure<UiActionResult>(UiErrors::ActionResultInvalid);
        return Result<UiActionResult>::Success(UiActionResult{UiActionResultKind::Handled, request});
    }

    /** @copydoc UiActionResult::Rejected */
    Result<UiActionResult> UiActionResult::Rejected(const UiActionRequestId request, const UiActionRejectionReason reason) {
        if (!request.IsValid() || !IsKnownEnum(reason, UiActionRejectionReason::Count))
            return Failure<UiActionResult>(UiErrors::ActionResultInvalid);
        UiActionResult result;
        result.kind = UiActionResultKind::Rejected;
        result.request = request;
        result.rejection = reason;
        return Result<UiActionResult>::Success(std::move(result));
    }

    /** @copydoc UiActionResult::Pending */
    Result<UiActionResult> UiActionResult::Pending(const UiActionRequestId request, const UiActionOperationId operation) {
        if (!request.IsValid())
            return Failure<UiActionResult>(UiErrors::ActionResultInvalid);
        if (const auto valid = ValidateOperation(operation, request); valid.HasError())
            return Result<UiActionResult>::Failure(valid.ErrorValue());
        UiActionResult result;
        result.kind = UiActionResultKind::Pending;
        result.request = request;
        result.operation = operation;
        result.hasOperation = true;
        return Result<UiActionResult>::Success(std::move(result));
    }

    /** @copydoc UiActionResult::Completed */
    Result<UiActionResult> UiActionResult::Completed(const UiActionRequestId request, UiActionPayload payload,
                                                     const std::optional<UiActionOperationId> operation) {
        if (!request.IsValid())
            return Failure<UiActionResult>(UiErrors::ActionResultInvalid);
        if (operation.has_value()) {
            if (const auto valid = ValidateOperation(*operation, request); valid.HasError())
                return Result<UiActionResult>::Failure(valid.ErrorValue());
        }
        if (const auto valid = payload.Validate(); valid.HasError())
            return Result<UiActionResult>::Failure(valid.ErrorValue());
        UiActionResult result;
        result.kind = UiActionResultKind::Completed;
        result.request = request;
        result.payload = std::move(payload);
        if (operation.has_value()) {
            result.operation = *operation;
            result.hasOperation = true;
        }
        return Result<UiActionResult>::Success(std::move(result));
    }

    /** @copydoc UiActionResult::CompletedNavigation */
    Result<UiActionResult> UiActionResult::CompletedNavigation(const UiActionRequestId request, UiDefaultNavigationResult navigation) {
        if (!request.IsValid())
            return Failure<UiActionResult>(UiErrors::ActionResultInvalid);
        if (!NavigationMatchesOwner(navigation, request.ownership))
            return Failure<UiActionResult>(UiErrors::NavigationInvalid);
        auto result = Completed(request);
        if (result.HasError())
            return result;
        UiActionResult completed = std::move(result).Value();
        completed.navigation = std::move(navigation);
        return Result<UiActionResult>::Success(std::move(completed));
    }

    /** @copydoc UiActionResult::Cancelled */
    Result<UiActionResult> UiActionResult::Cancelled(const UiActionRequestId request, const UiActionOperationId operation,
                                                     const UiActionCancellationReason reason) {
        if (!request.IsValid() || !IsKnownEnum(reason, UiActionCancellationReason::Count))
            return Failure<UiActionResult>(UiErrors::ActionResultInvalid);
        if (const auto valid = ValidateOperation(operation, request); valid.HasError())
            return Result<UiActionResult>::Failure(valid.ErrorValue());
        UiActionResult result;
        result.kind = UiActionResultKind::Cancelled;
        result.request = request;
        result.operation = operation;
        result.hasOperation = true;
        result.cancellation = reason;
        return Result<UiActionResult>::Success(std::move(result));
    }

    /** @copydoc UiActionResult::Validate */
    Result<void> UiActionResult::Validate() const {
        if (!request.IsValid() || !IsKnownEnum(kind, UiActionResultKind::Count))
            return Failure(UiErrors::ActionResultInvalid);
        if (hasOperation) {
            if (const auto valid = ValidateOperation(operation, request); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
        } else if (operation.IsValid()) {
            return Failure(UiErrors::ActionResultInvalid);
        }
        if (navigation.has_value() && (!NavigationMatchesOwner(*navigation, request.ownership) || kind != UiActionResultKind::Completed))
            return Failure(UiErrors::NavigationInvalid);

        switch (kind) {
            case UiActionResultKind::Handled:
                return !hasOperation && rejection == UiActionRejectionReason::Count && cancellation == UiActionCancellationReason::Count &&
                               payload.Size() == 0 && !navigation.has_value()
                           ? Result<void>::Success()
                           : Failure(UiErrors::ActionResultInvalid);
            case UiActionResultKind::Rejected:
                return !hasOperation && IsKnownEnum(rejection, UiActionRejectionReason::Count) &&
                               cancellation == UiActionCancellationReason::Count && payload.Size() == 0 && !navigation.has_value()
                           ? Result<void>::Success()
                           : Failure(UiErrors::ActionResultInvalid);
            case UiActionResultKind::Pending:
                return hasOperation && rejection == UiActionRejectionReason::Count && cancellation == UiActionCancellationReason::Count &&
                               payload.Size() == 0 && !navigation.has_value()
                           ? Result<void>::Success()
                           : Failure(UiErrors::ActionResultInvalid);
            case UiActionResultKind::Completed:
                if (rejection != UiActionRejectionReason::Count || cancellation != UiActionCancellationReason::Count)
                    return Failure(UiErrors::ActionResultInvalid);
                return payload.Validate();
            case UiActionResultKind::Cancelled:
                return hasOperation && rejection == UiActionRejectionReason::Count &&
                               IsKnownEnum(cancellation, UiActionCancellationReason::Count) && payload.Size() == 0 &&
                               !navigation.has_value()
                           ? Result<void>::Success()
                           : Failure(UiErrors::ActionResultInvalid);
            case UiActionResultKind::Count:
                break;
        }
        return Failure(UiErrors::ActionResultInvalid);
    }

    /** @copydoc UiActionResult::IsTerminal */
    bool UiActionResult::IsTerminal() const noexcept {
        return kind == UiActionResultKind::Rejected || kind == UiActionResultKind::Completed || kind == UiActionResultKind::Cancelled;
    }

    struct UiActionRouter::Storage final {
        explicit Storage(const UiActionRouterDescriptor &descriptor) : owner(descriptor.owner), queue(descriptor.maximumQueuedCommands) {}

        UiActionOwnerContext owner;
        std::vector<UiActionRequest> queue;
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
