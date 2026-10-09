#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiControlsInternal.h"

#include <cmath>
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
    Result<UiControlEventResult> UiControlStateMachine::Handle(const UiControlInput &input) {
        using enum UiControlInputKind;
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<UiControlEventResult>(UiErrors::ControlLifecycleUnavailable);
        if (!input.IsValid() || !UiControlDetail::IsPressSource(input.kind, input.activationSource) ||
            ((input.kind == TextInput) && !UiControlDetail::IsValidControlText(input.text)) ||
            (input.kind != TextInput && input.text.size != 0))
            return Failure<UiControlEventResult>(UiErrors::ControlInputInvalid);
        if (const UiControlDescriptorBase &base = UiControlDetail::BaseOf(storage_->descriptor);
            input.source.owner != base.owner || input.source.element != base.element)
            return Failure<UiControlEventResult>(UiErrors::ControlSourceStale);
        if (input.sequence <= storage_->lastSequence || (input.tick != 0 && input.tick < storage_->lastTick))
            return Failure<UiControlEventResult>(UiErrors::ControlSequenceInvalid);
        if (storage_->pending && input.kind != Cancel && input.kind != FocusLost)
            return Failure<UiControlEventResult>(UiErrors::ControlDefaultPending);
        const auto transition = storage_->ApplyInput(input);
        if (transition.HasError())
            return Result<UiControlEventResult>::Failure(transition.ErrorValue());
        storage_->lastSequence = input.sequence;
        if (input.tick != 0)
            storage_->lastTick = input.tick;
        return Result<UiControlEventResult>::Success({transition.Value(), storage_->state, storage_->pending});
    }

    /** @copydoc UiControlStateMachine::PeekDefault */
    Result<std::optional<UiControlDefaultAction>> UiControlStateMachine::PeekDefault() const {
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

        return Result<std::optional<UiControlDefaultAction>>::Success(std::optional<UiControlDefaultAction>{std::move(action)});
    }

    /** @copydoc UiControlStateMachine::ApplyDefault */
    Result<std::optional<UiControlDefaultAction>> UiControlStateMachine::ApplyDefault() {
        using enum UiControlDetail::PendingKind;
        auto action = PeekDefault();
        if (action.HasError() || !action.Value().has_value())
            return action;

        if (storage_->pendingDefault.kind == Toggle)
            std::get<UiToggleControlState>(storage_->state).checked = !std::get<UiToggleControlState>(storage_->state).checked;
        else if (storage_->pendingDefault.kind == ValueChanged)
            std::get<UiSliderControlState>(storage_->state).value = storage_->pendingDefault.value;
        else if (storage_->pendingDefault.kind == Submit && std::get<UiTextInputControlDescriptor>(storage_->descriptor).submitEndsEditing)
            std::get<UiTextInputControlState>(storage_->state).editing = false;

        storage_->pending = false;
        return action;
    }

    /** @copydoc UiControlStateMachine::SuppressDefault */
    Result<void> UiControlStateMachine::SuppressDefault() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        storage_->pending = false;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::ReconcileValue */
    Result<void> UiControlStateMachine::ReconcileValue(const UiActionValue &value) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        if (const auto reconciled = storage_->ReconcileValue(value); reconciled.HasError())
            return reconciled;
        storage_->ClearTransient(false);
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Storage::ReconcileValue */
    Result<void> UiControlStateMachine::Storage::ReconcileValue(const UiActionValue &value) {
        using enum UiControlKind;
        switch (UiControlDetail::KindOf(descriptor)) {
            case Toggle:
                return ReconcileToggle(value);
            case Slider:
                return ReconcileSlider(value);
            case TextInput:
                return ReconcileText(value);
            case Button:
            case Count:
                return Failure(UiErrors::ControlInputInvalid);
        }
        return Failure(UiErrors::ControlInputInvalid);
    }

    /** @copydoc UiControlStateMachine::Storage::ReconcileToggle */
    Result<void> UiControlStateMachine::Storage::ReconcileToggle(const UiActionValue &value) {
        const auto *checked = std::get_if<bool>(&value);
        if (!checked)
            return Failure(UiErrors::ControlInputInvalid);
        std::get<UiToggleControlState>(state).checked = *checked;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Storage::ReconcileSlider */
    Result<void> UiControlStateMachine::Storage::ReconcileSlider(const UiActionValue &value) {
        const auto *scalar = std::get_if<double>(&value);
        const auto &slider = std::get<UiSliderControlDescriptor>(descriptor);
        if (!scalar || !std::isfinite(*scalar) || *scalar < slider.minimum || *scalar > slider.maximum)
            return Failure(UiErrors::ControlInputInvalid);
        std::get<UiSliderControlState>(state).value = *scalar;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Storage::ReconcileText */
    Result<void> UiControlStateMachine::Storage::ReconcileText(const UiActionValue &value) {
        const auto *text = std::get_if<UiActionText>(&value);
        const auto &input = std::get<UiTextInputControlDescriptor>(descriptor);
        if (!text || !UiControlDetail::IsValidControlText(*text) || text->size > input.maximumTextBytes)
            return Failure(UiErrors::ControlInputInvalid);
        if (const auto valid = textEditor->Reset(*text); valid.HasError())
            return valid;
        std::get<UiTextInputControlState>(state).text = *text;
        editStartText = *text;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Storage::AdmitTextEdit */
    Result<void> UiControlStateMachine::Storage::AdmitTextEdit(const UiActionSource &source, const std::uint64_t sequence) const {
        if (!textEditor)
            return Failure(UiErrors::ControlInputInvalid);
        const auto &base = UiControlDetail::BaseOf(descriptor);
        if (source.owner != base.owner || source.element != base.element)
            return Failure(UiErrors::ControlSourceStale);
        if (sequence == 0 || sequence <= lastSequence)
            return Failure(UiErrors::ControlSequenceInvalid);
        if (pending)
            return Failure(UiErrors::ControlDefaultPending);
        const auto &input = std::get<UiTextInputControlState>(state);
        if (!UiControlDetail::IsEnabled(state) || !input.focused || !input.editing)
            return Failure(UiErrors::ControlInputInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::EditText */
    Result<UiTextEditResult> UiControlStateMachine::EditText(const UiActionSource &source, const std::uint64_t sequence,
                                                             const UiTextEditCommand &command) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<UiTextEditResult>(UiErrors::ControlLifecycleUnavailable);
        if (const auto admitted = storage_->AdmitTextEdit(source, sequence); admitted.HasError())
            return Result<UiTextEditResult>::Failure(admitted.ErrorValue());
        auto result = storage_->textEditor->Apply(command);
        if (result.HasError())
            return result;
        std::get<UiTextInputControlState>(storage_->state).text = storage_->textEditor->Snapshot().Value().text;
        storage_->lastSequence = sequence;
        return result;
    }

    /** @copydoc UiControlStateMachine::TextEditSnapshot */
    Result<UiTextEditSnapshot> UiControlStateMachine::TextEditSnapshot() const {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<UiTextEditSnapshot>(UiErrors::ControlLifecycleUnavailable);
        if (!storage_->textEditor)
            return Failure<UiTextEditSnapshot>(UiErrors::ControlInputInvalid);
        return storage_->textEditor->Snapshot();
    }

    /** @copydoc UiControlStateMachine::TextDisplay */
    Result<UiTextEditDisplay> UiControlStateMachine::TextDisplay() const {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<UiTextEditDisplay>(UiErrors::ControlLifecycleUnavailable);
        if (!storage_->textEditor)
            return Failure<UiTextEditDisplay>(UiErrors::ControlInputInvalid);
        return storage_->textEditor->Display();
    }

    /** @copydoc UiControlStateMachine::SetAvailability */
    Result<void> UiControlStateMachine::SetAvailability(const UiControlAvailability availability) {
        using enum UiControlAvailability;
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        if (!IsKnown(availability, Count) || availability == Busy)
            return Failure(UiErrors::ControlInputInvalid);
        storage_->configuredAvailability = availability;
        const auto effective = availability == Enabled && storage_->asyncAction && storage_->asyncAction->Busy() ? Busy : availability;
        if (availability == Disabled) {
            storage_->ClearTransient(true);
            storage_->SetAvailabilityProjection(effective);
        } else {
            storage_->SetAvailabilityProjection(effective);
        }
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::ObserveAsyncActions */
    Result<void> UiControlStateMachine::ObserveAsyncActions(const UiAsyncActionStore &actions) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        const auto &base = UiControlDetail::BaseOf(storage_->descriptor);
        auto projected = actions.Project({base.owner, base.element});
        if (projected.HasError())
            return Result<void>::Failure(projected.ErrorValue());
        const bool busy = projected.Value() && projected.Value()->Busy();
        if (busy)
            storage_->ClearTransient(false);
        storage_->asyncAction = std::move(projected).Value();
        const auto effective = storage_->configuredAvailability == UiControlAvailability::Enabled && busy
                                   ? UiControlAvailability::Busy
                                   : storage_->configuredAvailability;
        storage_->SetAvailabilityProjection(effective);
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::AsyncAction */
    Result<std::optional<UiAsyncActionSnapshot>> UiControlStateMachine::AsyncAction() const {
        if (!storage_ || storage_->lifecycle == UiControlLifecycleState::Stopped)
            return Failure<std::optional<UiAsyncActionSnapshot>>(UiErrors::ControlLifecycleUnavailable);
        return Result<std::optional<UiAsyncActionSnapshot>>::Success(storage_->asyncAction);
    }

    /** @copydoc UiControlStateMachine::BeginRetirement */
    Result<void> UiControlStateMachine::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        storage_->ClearTransient(true);
        storage_->lifecycle = UiControlLifecycleState::Retiring;
        storage_->asyncAction.reset();
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Shutdown */
    void UiControlStateMachine::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiControlLifecycleState::Stopped)
            return;
        storage_->ClearTransient(true);
        storage_->SetAvailabilityProjection(UiControlAvailability::Disabled);
        storage_->lifecycle = UiControlLifecycleState::Stopped;
        storage_->asyncAction.reset();
        if (storage_->textEditor) {
            storage_->textEditor->Shutdown();
            std::get<UiTextInputControlState>(storage_->state).text = {};
            storage_->editStartText = {};
        }
        if (replacement_ && replacement_->textEditor) {
            replacement_->textEditor->Shutdown();
            std::get<UiTextInputControlState>(replacement_->state).text = {};
            replacement_->editStartText = {};
        }
    }
}  // namespace Horo::Runtime::Ui
