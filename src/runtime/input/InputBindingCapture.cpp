#include "Horo/Runtime/Input.h"
#include "InputErrors.h"
#include "InputRouterInternal.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::Input {
    namespace {
        /** @brief Finds a frame-local physical button edge in the bounded canonical or raw button span. */
        std::optional<std::size_t> PressedButton(const std::span<const ButtonState> buttons) {
            const auto found = std::ranges::find_if(buttons, [](const ButtonState &state) {
                return state.pressed;
            });
            return found == buttons.end() ? std::nullopt : std::optional{static_cast<std::size_t>(found - buttons.begin())};
        }

        /** @brief Arms near-center axes and requires deliberate motion from a stored baseline. */
        std::optional<std::size_t> MovedAxis(const std::span<const float> current, const std::span<float> baseline) {
            for (std::size_t index = 0; index < current.size(); ++index) {
                if (std::abs(current[index]) < 0.25F)
                    baseline[index] = current[index];
                if (std::abs(current[index]) >= 0.75F && std::abs(current[index] - baseline[index]) >= 0.5F)
                    return index;
            }
            return std::nullopt;
        }

        /** @brief Orients an analog capture gesture toward a positive output; Digital controls remain boolean. */
        float CaptureScale(const float value, const ActionValueType valueType) noexcept {
            return valueType != ActionValueType::Digital && value < 0.0F ? -1.0F : 1.0F;
        }

        /** @brief Selects canonical controls before raw fallback without using adapter display labels as identity. */
        std::optional<InputBinding> GamepadBinding(const GamepadState &gamepad, GamepadState &baseline, const ActionValueType valueType) {
            if (const auto button = PressedButton(gamepad.buttons))
                return InputBinding{.kind = BindingControlKind::GamepadButton, .gamepadButton = static_cast<GamepadButton>(*button)};
            if (const auto button = PressedButton(gamepad.rawButtons))
                return InputBinding{.kind = BindingControlKind::RawGamepadButton, .rawControl = static_cast<std::uint16_t>(*button)};
            if (const auto axis = MovedAxis(gamepad.axes, baseline.axes))
                return InputBinding{.kind = BindingControlKind::GamepadAxis,
                                    .gamepadAxis = static_cast<GamepadAxis>(*axis),
                                    .scale = CaptureScale(gamepad.axes[*axis], valueType),
                                    .deadzoneKind = DeadzoneKind::Axial,
                                    .deadzone = 0.15F};
            if (const auto axis = MovedAxis(gamepad.rawAxes, baseline.rawAxes))
                return InputBinding{.kind = BindingControlKind::RawGamepadAxis,
                                    .rawControl = static_cast<std::uint16_t>(*axis),
                                    .scale = CaptureScale(gamepad.rawAxes[*axis], valueType),
                                    .deadzoneKind = DeadzoneKind::Axial,
                                    .deadzone = 0.15F};
            return std::nullopt;
        }

        /** @brief Selects one typed control after lifecycle admission, with deterministic keyboard/pointer/gamepad priority. */
        std::optional<InputBinding> SnapshotBinding(const RawInputSnapshot &snapshot, std::vector<GamepadState> &baselines,
                                                    const ActionValueType valueType) {
            for (std::size_t index = 1; index < snapshot.keyboard.size(); ++index)
                if (snapshot.keyboard[index].pressed)
                    return InputBinding{.kind = BindingControlKind::Key,
                                        .key = static_cast<Key>(index),
                                        .requiredModifiers = snapshot.modifiers};
            if (const auto button = PressedButton(snapshot.pointer.buttons))
                return InputBinding{.kind = BindingControlKind::PointerButton,
                                    .pointerButton = static_cast<PointerButton>(*button),
                                    .requiredModifiers = snapshot.modifiers};
            if (snapshot.pointer.wheelX != 0.0F)
                return InputBinding{.kind = BindingControlKind::PointerWheelX,
                                    .requiredModifiers = snapshot.modifiers,
                                    .scale = CaptureScale(snapshot.pointer.wheelX, valueType)};
            if (snapshot.pointer.wheelY != 0.0F)
                return InputBinding{.kind = BindingControlKind::PointerWheelY,
                                    .requiredModifiers = snapshot.modifiers,
                                    .scale = CaptureScale(snapshot.pointer.wheelY, valueType)};
            for (const auto &pad : snapshot.gamepads) {
                auto &baseline = *std::ranges::find(baselines, pad.id, &GamepadState::id);
                if (const auto binding = GamepadBinding(pad, baseline, valueType))
                    return binding;
            }
            return std::nullopt;
        }

        /** @brief Checks exact device generations and raw shapes before indexing stored baselines. */
        bool SameDevices(const RawInputSnapshot &snapshot, const std::vector<GamepadState> &baselines) {
            return snapshot.gamepads.size() == baselines.size() && std::ranges::all_of(snapshot.gamepads, [&](const GamepadState &pad) {
                const auto baseline = std::ranges::find(baselines, pad.id, &GamepadState::id);
                return baseline != baselines.end() && baseline->rawAxes.size() == pad.rawAxes.size() &&
                       baseline->rawButtons.size() == pad.rawButtons.size();
            });
        }
    }  // namespace

    /** @copydoc InputBindingCapture::InputBindingCapture */
    InputBindingCapture::InputBindingCapture(InputBindingCapture &&other) noexcept
        : routerIdentity_(std::exchange(other.routerIdentity_, nullptr)), routerLifetime_(std::move(other.routerLifetime_)),
          routing_(other.routing_), valueType_(other.valueType_), frame_(other.frame_), terminal_(std::exchange(other.terminal_, true)),
          baselines_(std::move(other.baselines_)) {}

    /** @copydoc InputBindingCapture::operator= */
    InputBindingCapture &InputBindingCapture::operator=(InputBindingCapture &&other) noexcept {
        if (this != &other) {
            routerIdentity_ = std::exchange(other.routerIdentity_, nullptr);
            routerLifetime_ = std::move(other.routerLifetime_);
            routing_ = other.routing_;
            valueType_ = other.valueType_;
            frame_ = other.frame_;
            terminal_ = std::exchange(other.terminal_, true);
            baselines_ = std::move(other.baselines_);
        }
        return *this;
    }

    /** @copydoc InputBindingCapture::Create */
    Result<InputBindingCapture> InputBindingCapture::Create(const InputRouter &router, const InputContextToken &context,
                                                            const ActionId &action) {
        if (const auto owner = router.impl_->collection->Borrow(); owner.HasError())
            return Result<InputBindingCapture>::Failure(owner.ErrorValue());
        if (!router.IsContextActive(context))
            return Result<InputBindingCapture>::Failure(MakeError(Errors::CaptureInactiveContext));
        const auto descriptor = std::ranges::find(router.Actions(), action, &ActionDescriptor::id);
        if (descriptor == router.Actions().end())
            return Result<InputBindingCapture>::Failure(
                MakeError(Errors::ProfileValidationFailed, "Rebinding requires a registered action."));
        InputBindingCapture capture;
        capture.routerIdentity_ = &router;
        capture.routerLifetime_ = router.impl_->collection;
        capture.routing_ = router.RoutingState(context);
        capture.valueType_ = descriptor->valueType;
        capture.frame_ = router.Snapshot().frame;
        capture.baselines_ = router.Snapshot().gamepads;
        return Result<InputBindingCapture>::Success(std::move(capture));
    }

    /** @copydoc InputBindingCapture::Poll */
    BindingCaptureResult InputBindingCapture::Poll(InputRouter &router, const InputContextToken &context) {
        if (terminal_)
            return {BindingCaptureStatus::Cancelled, {}};
        const auto lifetime = routerLifetime_.lock();
        if (!lifetime || &router != routerIdentity_ || lifetime->Borrow().HasError()) {
            terminal_ = true;
            return {BindingCaptureStatus::Cancelled, {}};
        }
        const auto routing = router.RoutingState(context);
        const auto &snapshot = router.Snapshot();
        if (!router.IsContextActive(context) || !snapshot.window.focused || routing.contextIdentity != routing_.contextIdentity ||
            routing.configurationRevision != routing_.configurationRevision || routing.assignmentRevision != routing_.assignmentRevision ||
            !SameDevices(snapshot, baselines_)) {
            terminal_ = true;
            router.FinishBindingCapture();
            return {BindingCaptureStatus::Cancelled, {}};
        }
        if (snapshot.frame == frame_)
            return {};
        frame_ = snapshot.frame;
        if (snapshot.State(Key::Escape).pressed) {
            terminal_ = true;
            router.FinishBindingCapture();
            return {BindingCaptureStatus::Cancelled, {}};
        }
        const auto binding = SnapshotBinding(snapshot, baselines_, valueType_);
        terminal_ = binding.has_value();
        if (terminal_)
            router.FinishBindingCapture();
        return {binding ? BindingCaptureStatus::Captured : BindingCaptureStatus::Pending, binding};
    }
}  // namespace Horo::Input
