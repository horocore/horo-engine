#include "UiAnimationOwnerInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Projects actual typed control availability and activation without granting focus or input authority. */
        [[nodiscard]] UiVisualStateMask ControlVisualState(const UiControlState &state) noexcept {
            return std::visit([](const auto &typed) noexcept {
                UiVisualStateMask result;
                if (typed.availability == UiControlAvailability::Disabled)
                    result = result | UiVisualState::Disabled;
                if (typed.availability == UiControlAvailability::Busy)
                    result = result | UiVisualState::Busy;
                if (typed.pressed)
                    result = result | UiVisualState::Pressed;
                if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, UiToggleControlState>) {
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
        for (auto &input : storage.elementInputs)
            input.state = {};
        for (const auto &control : canvas->controls) {
            auto state = control.control.Snapshot();
            if (state.HasError())
                return Result<void>::Failure(state.ErrorValue());
            const auto input = std::ranges::find(storage.elementInputs, control.control.Element(), &UiStyleElementInput::element);
            if (input == storage.elementInputs.end())
                return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
            input->state = ControlVisualState(state.Value());
        }
        if (canvas->focus) {
            const auto focused = canvas->focus->CurrentFocus();
            if (focused.HasError())
                return Result<void>::Failure(focused.ErrorValue());
            if (focused.Value()) {
                const auto input = std::ranges::find(storage.elementInputs, focused.Value()->element, &UiStyleElementInput::element);
                if (input == storage.elementInputs.end())
                    return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
                input->state = input->state | UiVisualState::Focused;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
