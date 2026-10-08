#include "UiAnimationOwnerInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Projects actual typed control availability and activation without granting focus or input authority. */
        [[nodiscard]] UiVisualStateMask ControlVisualState(const UiControlState &state) noexcept {
            return std::visit([]<typename Control>(const Control &typed) noexcept {
                UiVisualStateMask result;
                if (typed.availability == UiControlAvailability::Disabled)
                    result = result | UiVisualState::Disabled;
                if (typed.availability == UiControlAvailability::Busy)
                    result = result | UiVisualState::Busy;
                if (typed.pressed)
                    result = result | UiVisualState::Pressed;
                if constexpr (std::is_same_v<Control, UiToggleControlState>) {
                    if (typed.checked)
                        result = result | UiVisualState::Checked;
                }
                return result;
            }, state);
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::PrepareVisualState */
    Result<void> UiAnimationOwner::PrepareVisualState(Storage &storage) {
        const auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        for (auto &input : storage.work.elementInputs)
            input.state = {};
        for (const auto &control : canvas->controls) {
            auto state = control.control.Snapshot();
            if (state.HasError())
                return Result<void>::Failure(state.ErrorValue());
            const auto input = std::ranges::find(storage.work.elementInputs, control.control.Element(), &UiStyleElementInput::element);
            if (input == storage.work.elementInputs.end())
                return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
            input->state = ControlVisualState(state.Value());
        }
        if (canvas->focus) {
            const auto focused = canvas->focus->CurrentFocus();
            if (focused.HasError())
                return Result<void>::Failure(focused.ErrorValue());
            if (focused.Value()) {
                const auto input = std::ranges::find(storage.work.elementInputs, focused.Value()->element, &UiStyleElementInput::element);
                if (input == storage.work.elementInputs.end())
                    return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
                input->state = input->state | UiVisualState::Focused;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
