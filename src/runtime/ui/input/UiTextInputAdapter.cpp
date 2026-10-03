#include "UiTextInputAdapter.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <utility>

namespace Horo::Runtime::Ui::InputAdapter {
    /** @copydoc DeliverFocusedText */
    Result<std::optional<Input::TextInputDelivery>> DeliverFocusedText(Input::InputRouter &router, const Input::InputContextToken &context,
                                                                       UiControlStateMachine &control, const UiActionSource &source,
                                                                       const std::uint64_t sequence) {
        using Delivery = std::optional<Input::TextInputDelivery>;
        if (control.Kind() != UiControlKind::TextInput || !source.IsValid() || source.owner != control.Owner() ||
            source.element != control.Element())
            return Result<Delivery>::Failure(MakeError(UiErrors::ControlInputInvalid));
        const auto state = control.Snapshot();
        if (state.HasError())
            return Result<Delivery>::Failure(state.ErrorValue());
        if (const auto &textState = std::get<UiTextInputControlState>(state.Value());
            textState.availability != UiControlAvailability::Enabled || !textState.focused || !textState.editing)
            return Result<Delivery>::Success(std::nullopt);
        Delivery delivery = router.TakeText(context);
        if (!delivery || delivery->committed.empty())
            return Result<Delivery>::Success(std::move(delivery));
        auto payload = UiActionText::Create(delivery->committed);
        if (payload.HasError())
            return Result<Delivery>::Failure(payload.ErrorValue());
        const UiControlInput input{source, UiControlInputKind::TextInput, UiControlActivationSource::Keyboard, sequence,
                                   0,      UiControlAdjustment::Count,    std::move(payload).Value()};
        if (const auto applied = control.Handle(input); applied.HasError())
            return Result<Delivery>::Failure(applied.ErrorValue());
        return Result<Delivery>::Success(std::move(delivery));
    }
}  // namespace Horo::Runtime::Ui::InputAdapter
