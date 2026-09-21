#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiControlsInternal.h"

#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnown(const Enum value, const Enum count) noexcept {
            using Underlying = std::underlying_type_t<Enum>;
            return static_cast<Underlying>(value) < static_cast<Underlying>(count);
        }
    }  // namespace

    namespace UiControlDetail {
        [[nodiscard]] const UiActionOwnerContext &InvalidOwner() noexcept {
            static const UiActionOwnerContext invalid{};
            return invalid;
        }

        [[nodiscard]] UiElementHandle InvalidElement() noexcept {
            return {};
        }
    }  // namespace UiControlDetail

    /** @copydoc UiControlStateMachine::Create */
    Result<UiControlStateMachine> UiControlStateMachine::Create(const UiControlDescriptor &descriptor) {
        if (const auto valid = ValidateUiControlDescriptor(descriptor); valid.HasError())
            return Result<UiControlStateMachine>::Failure(valid.ErrorValue());
        try {
            return Result<UiControlStateMachine>::Success(UiControlStateMachine{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiControlStateMachine>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiControlStateMachine::UiControlStateMachine */
    UiControlStateMachine::UiControlStateMachine(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiControlStateMachine::~UiControlStateMachine */
    UiControlStateMachine::~UiControlStateMachine() {
        Shutdown();
    }

    /** @copydoc UiControlStateMachine::UiControlStateMachine */
    UiControlStateMachine::UiControlStateMachine(UiControlStateMachine &&) noexcept = default;

    /** @copydoc UiControlStateMachine::operator= */
    UiControlStateMachine &UiControlStateMachine::operator=(UiControlStateMachine &&) noexcept = default;

    /** @copydoc UiControlStateMachine::Kind */
    UiControlKind UiControlStateMachine::Kind() const noexcept {
        return storage_ ? UiControlDetail::KindOf(storage_->descriptor) : UiControlKind::Count;
    }

    /** @copydoc UiControlStateMachine::Owner */
    const UiActionOwnerContext &UiControlStateMachine::Owner() const noexcept {
        return storage_ ? UiControlDetail::BaseOf(storage_->descriptor).owner : UiControlDetail::InvalidOwner();
    }

    /** @copydoc UiControlStateMachine::Element */
    UiElementHandle UiControlStateMachine::Element() const noexcept {
        return storage_ ? UiControlDetail::BaseOf(storage_->descriptor).element : UiControlDetail::InvalidElement();
    }

    /** @copydoc UiControlStateMachine::LifecycleState */
    UiControlLifecycleState UiControlStateMachine::LifecycleState() const noexcept {
        return storage_ ? storage_->lifecycle : UiControlLifecycleState::Stopped;
    }

    /** @copydoc UiControlStateMachine::Snapshot */
    Result<UiControlState> UiControlStateMachine::Snapshot() const {
        if (!storage_ || storage_->lifecycle == UiControlLifecycleState::Stopped)
            return Failure<UiControlState>(UiErrors::ControlLifecycleUnavailable);
        return Result<UiControlState>::Success(storage_->state);
    }

    /** @copydoc UiControlStateMachine::Handle */
    Result<UiControlEventResult> UiControlStateMachine::Handle(UiControlInput input) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<UiControlEventResult>(UiErrors::ControlLifecycleUnavailable);
        if (!input.IsValid() || !UiControlDetail::IsPressSource(input.kind, input.activationSource) ||
            ((input.kind == UiControlInputKind::TextInput) && !UiControlDetail::IsValidControlText(input.text)) ||
            (input.kind != UiControlInputKind::TextInput && input.text.size != 0))
            return Failure<UiControlEventResult>(UiErrors::ControlInputInvalid);
        const UiControlDescriptorBase &base = UiControlDetail::BaseOf(storage_->descriptor);
        if (input.source.owner != base.owner || input.source.element != base.element)
            return Failure<UiControlEventResult>(UiErrors::ControlSourceStale);
        if (input.sequence <= storage_->lastSequence || (input.tick != 0 && input.tick < storage_->lastTick))
            return Failure<UiControlEventResult>(UiErrors::ControlSequenceInvalid);
        if (storage_->pending && input.kind != UiControlInputKind::Cancel && input.kind != UiControlInputKind::FocusLost)
            return Failure<UiControlEventResult>(UiErrors::ControlDefaultPending);
        const auto transition = storage_->ApplyInput(input);
        if (transition.HasError())
            return Result<UiControlEventResult>::Failure(transition.ErrorValue());
        storage_->lastSequence = input.sequence;
        if (input.tick != 0)
            storage_->lastTick = input.tick;
        return Result<UiControlEventResult>::Success({transition.Value(), storage_->state, storage_->pending});
    }

    /** @copydoc UiControlStateMachine::ApplyDefault */
    Result<std::optional<UiControlDefaultAction>> UiControlStateMachine::ApplyDefault() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlLifecycleUnavailable);
        if (!storage_->pending)
            return Result<std::optional<UiControlDefaultAction>>::Success(std::nullopt);

        const UiControlDescriptorBase &base = UiControlDetail::BaseOf(storage_->descriptor);
        UiControlDefaultAction action;
        action.source = {base.owner, base.element};
        action.action = base.action;
        action.activationSource = storage_->pendingDefault.source;
        action.eventSequence = storage_->pendingDefault.sequence;
        action.repeated = storage_->pendingDefault.repeated;
        action.payload = base.payload;

        switch (storage_->pendingDefault.kind) {
            case UiControlDetail::PendingKind::Activate:
                action.kind = UiControlActionKind::Activate;
                break;
            case UiControlDetail::PendingKind::Toggle: {
                action.kind = UiControlActionKind::Toggle;
                const bool next = !std::get<UiToggleControlState>(storage_->state).checked;
                if (const auto added = action.payload.Add(next); added.HasError())
                    return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);
                break;
            }
            case UiControlDetail::PendingKind::ValueChanged:
                action.kind = UiControlActionKind::ValueChanged;
                if (const auto added = action.payload.Add(storage_->pendingDefault.value); added.HasError())
                    return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);
                break;
            case UiControlDetail::PendingKind::Submit:
                action.kind = UiControlActionKind::Submit;
                if (const auto added = action.payload.Add(std::get<UiTextInputControlState>(storage_->state).text); added.HasError())
                    return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);
                break;
        }
        if (!action.IsValid())
            return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);

        if (storage_->pendingDefault.kind == UiControlDetail::PendingKind::Toggle)
            std::get<UiToggleControlState>(storage_->state).checked = !std::get<UiToggleControlState>(storage_->state).checked;
        else if (storage_->pendingDefault.kind == UiControlDetail::PendingKind::ValueChanged)
            std::get<UiSliderControlState>(storage_->state).value = storage_->pendingDefault.value;
        else if (storage_->pendingDefault.kind == UiControlDetail::PendingKind::Submit &&
                 std::get<UiTextInputControlDescriptor>(storage_->descriptor).submitEndsEditing)
            std::get<UiTextInputControlState>(storage_->state).editing = false;

        storage_->pending = false;
        return Result<std::optional<UiControlDefaultAction>>::Success(std::optional<UiControlDefaultAction>{std::move(action)});
    }

    /** @copydoc UiControlStateMachine::SuppressDefault */
    Result<void> UiControlStateMachine::SuppressDefault() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        storage_->pending = false;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::SetAvailability */
    Result<void> UiControlStateMachine::SetAvailability(const UiControlAvailability availability) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        if (!IsKnown(availability, UiControlAvailability::Count))
            return Failure(UiErrors::ControlInputInvalid);
        if (availability == UiControlAvailability::Disabled) {
            storage_->ClearTransient(true);
            storage_->SetAvailabilityProjection(availability);
        } else {
            storage_->SetAvailabilityProjection(availability);
        }
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::BeginRetirement */
    Result<void> UiControlStateMachine::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        storage_->ClearTransient(true);
        storage_->lifecycle = UiControlLifecycleState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Shutdown */
    void UiControlStateMachine::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiControlLifecycleState::Stopped)
            return;
        storage_->ClearTransient(true);
        storage_->SetAvailabilityProjection(UiControlAvailability::Disabled);
        storage_->lifecycle = UiControlLifecycleState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
