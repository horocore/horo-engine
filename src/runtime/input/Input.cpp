#include "Horo/Runtime/Input.h"

#include "InputErrors.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Horo::Input {
    namespace {
        template <typename Enum> constexpr std::size_t Index(Enum value) noexcept {
            return static_cast<std::size_t>(value);
        }

        void Advance(ButtonState &state) noexcept {
            state.pressed = false;
            state.released = false;
        }

        void Set(ButtonState &state, const bool down) noexcept {
            if (state.down == down)
                return;
            state.down = down;
            state.pressed = down;
            state.released = !down;
        }

        float ApplyDeadzone(float value, const InputBinding &binding) noexcept {
            using enum DeadzoneKind;
            if (!std::isfinite(value))
                return 0.0F;
            const float deadzone = std::clamp(binding.deadzone, 0.0F, 0.99F);
            if (binding.deadzoneKind == Threshold)
                return std::abs(value) >= deadzone ? value : 0.0F;
            if (binding.deadzoneKind == Axial || binding.deadzoneKind == Radial) {
                if (std::abs(value) <= deadzone)
                    return 0.0F;
                return std::copysign((std::abs(value) - deadzone) / (1.0F - deadzone), value);
            }
            return std::clamp(value, -1.0F, 1.0F);
        }

        bool IsControlSupported(const InputBinding &binding) noexcept {
            using enum BindingControlKind;
            switch (binding.kind) {
                case Key:
                    return binding.key > Key::Unknown && binding.key < Key::Count;
                case PointerButton:
                    return binding.pointerButton < PointerButton::Count;
                case PointerWheelX:
                case PointerWheelY:
                    return true;
                case GamepadButton:
                    return binding.gamepadButton < GamepadButton::Count;
                case GamepadAxis:
                    return binding.gamepadAxis < GamepadAxis::Count;
                case RawGamepadButton:
                case RawGamepadAxis:
                    return binding.rawControl < 256;
            }
            return false;
        }

        bool IsReservedShortcut(const InputBinding &binding) noexcept {
            using enum Key;
            if (binding.kind != BindingControlKind::Key)
                return false;
            const ModifierState mods = binding.requiredModifiers;
            return (mods.command && !mods.control && !mods.alt && (binding.key == Q || binding.key == W)) ||
                   (mods.alt && binding.key == F4) || (mods.control && mods.alt && binding.key == Delete);
        }

        bool HasValidChordSize(const InputBinding &binding) noexcept {
            return binding.chordSize <= binding.chord.size();
        }

        bool SameTransition(const InputBinding &left, const InputBinding &right) noexcept {
            if (!HasValidChordSize(left) || !HasValidChordSize(right))
                return false;
            return left.kind == right.kind && left.key == right.key && left.pointerButton == right.pointerButton &&
                   left.gamepadButton == right.gamepadButton && left.gamepadAxis == right.gamepadAxis &&
                   left.rawControl == right.rawControl && left.requiredModifiers == right.requiredModifiers &&
                   left.chordSize == right.chordSize &&
                   std::equal(left.chord.begin(), left.chord.begin() + left.chordSize, right.chord.begin());
        }

        bool ChordsOverlap(const InputBinding &left, const InputBinding &right) noexcept {
            if (!HasValidChordSize(left) || !HasValidChordSize(right))
                return false;
            if (left.kind != right.kind || left.key != right.key || left.pointerButton != right.pointerButton ||
                left.gamepadButton != right.gamepadButton || left.gamepadAxis != right.gamepadAxis || left.rawControl != right.rawControl ||
                left.requiredModifiers != right.requiredModifiers)
                return false;
            const auto contains = [](const InputBinding &larger, const InputBinding &smaller) {
                return std::all_of(smaller.chord.begin(), smaller.chord.begin() + smaller.chordSize, [&](const Key key) {
                    return std::find(larger.chord.begin(), larger.chord.begin() + larger.chordSize, key) !=
                           larger.chord.begin() + larger.chordSize;
                });
            };
            return contains(left, right) || contains(right, left);
        }

        bool SameAnalogAxis(const InputBinding &left, const InputBinding &right) noexcept {
            if (left.kind != right.kind || left.requiredModifiers != right.requiredModifiers || left.chordSize != 0 || right.chordSize != 0)
                return false;
            using enum BindingControlKind;
            if (left.kind == GamepadAxis)
                return left.gamepadAxis == right.gamepadAxis;
            if (left.kind == RawGamepadAxis)
                return left.rawControl == right.rawControl;
            if (left.kind == PointerWheelX || left.kind == PointerWheelY)
                return true;
            return false;
        }

        struct BindingEvaluationResult {
            float axis{0.0F};
            ButtonState state{};
        };

        float ResolveGamepadAxis(const float rawAxis, const InputBinding &binding) noexcept {
            return binding.deadzoneKind == DeadzoneKind::Radial ? std::clamp(rawAxis, -1.0F, 1.0F) : ApplyDeadzone(rawAxis, binding);
        }

        /** @brief Derives pressed/released edges for a digital control driven by an analog axis. */
        ButtonState DigitalStateFromAxis(const float current, const float previous, const float threshold) noexcept {
            ButtonState state;
            state.down = std::abs(current) >= threshold;
            const bool previousDown = std::abs(previous) >= threshold;
            state.pressed = state.down && !previousDown;
            state.released = !state.down && previousDown;
            return state;
        }

        /** @brief Resolves the previous-frame value of a gamepad axis, tolerating missing or shrunken snapshots. */
        float PreviousAxisValue(const RawInputSnapshot *previousSnapshot, const GamepadDeviceId padId, const std::size_t controlIndex,
                                const InputBinding &binding, const bool raw) noexcept {
            const GamepadState *previous = previousSnapshot != nullptr ? previousSnapshot->FindGamepad(padId) : nullptr;
            if (previous == nullptr)
                return 0.0F;
            if (raw)
                return controlIndex < previous->rawAxes.size() ? ResolveGamepadAxis(previous->rawAxes[controlIndex], binding) : 0.0F;
            return controlIndex < previous->axes.size() ? ResolveGamepadAxis(previous->axes[controlIndex], binding) : 0.0F;
        }

        BindingEvaluationResult EvaluateBindingOnPad(const InputBinding &binding, const GamepadState &pad,
                                                     const RawInputSnapshot *previousSnapshot) noexcept {
            using enum BindingControlKind;
            switch (binding.kind) {
                case GamepadButton: {
                    const ButtonState state = pad.buttons[Index(binding.gamepadButton)];
                    return {state.down ? 1.0F : 0.0F, state};
                }
                case GamepadAxis: {
                    const float axis = ResolveGamepadAxis(pad.axes[Index(binding.gamepadAxis)], binding);
                    const float previous = PreviousAxisValue(previousSnapshot, pad.id, Index(binding.gamepadAxis), binding, false);
                    return {axis, DigitalStateFromAxis(axis, previous, binding.digitalThreshold)};
                }
                case RawGamepadButton: {
                    if (binding.rawControl >= pad.rawButtons.size())
                        break;
                    const ButtonState state = pad.rawButtons[binding.rawControl];
                    return {state.down ? 1.0F : 0.0F, state};
                }
                case RawGamepadAxis: {
                    if (binding.rawControl >= pad.rawAxes.size())
                        break;
                    const float axis = ResolveGamepadAxis(pad.rawAxes[binding.rawControl], binding);
                    const float previous = PreviousAxisValue(previousSnapshot, pad.id, binding.rawControl, binding, true);
                    return {axis, DigitalStateFromAxis(axis, previous, binding.digitalThreshold)};
                }
                default:
                    break;
            }
            return {};
        }

        /** @brief Stable per-control identifier used to dedupe pressed transitions across actions. */
        std::uint64_t GamepadControlId(const InputBinding &binding) noexcept {
            using enum BindingControlKind;
            switch (binding.kind) {
                case GamepadButton:
                    return Index(binding.gamepadButton);
                case RawGamepadButton:
                case RawGamepadAxis:
                    return binding.rawControl;
                default:
                    return Index(binding.gamepadAxis);
            }
        }

        BindingEvaluationResult EvaluateGamepadBinding(const InputBinding &binding, const RawInputSnapshot &snapshot,
                                                       const RawInputSnapshot *previousSnapshot, const InputDeviceAssignments &assignments,
                                                       const std::optional<PlayerId> &player,
                                                       std::unordered_set<std::uint64_t> &consumedGamepadTransitions) {
            BindingEvaluationResult result;
            for (const GamepadState &pad : snapshot.gamepads) {
                if (player.has_value() && assignments.PlayerFor(pad.id) != player)
                    continue;
                result = EvaluateBindingOnPad(binding, pad, previousSnapshot);
                if (result.state.pressed) {
                    const std::uint64_t control =
                        (static_cast<std::uint64_t>(binding.kind) << 48U) | (GamepadControlId(binding) << 32U) | pad.id.slot;
                    const std::uint64_t transition = control ^ pad.id.sessionGeneration;
                    if (!consumedGamepadTransitions.insert(transition).second)
                        result.state.pressed = false;
                }
                if (result.axis != 0.0F || result.state.pressed || result.state.released)
                    break;
            }
            return result;
        }
    }  // namespace

    const ButtonState &RawInputSnapshot::State(const Key key) const noexcept {
        static const ButtonState empty{};
        return Index(key) < keyboard.size() ? keyboard[Index(key)] : empty;
    }

    const ButtonState &RawInputSnapshot::State(const PointerButton button) const noexcept {
        static const ButtonState empty{};
        return Index(button) < pointer.buttons.size() ? pointer.buttons[Index(button)] : empty;
    }

    const GamepadState *RawInputSnapshot::FindGamepad(const GamepadDeviceId id) const noexcept {
        const auto found = std::ranges::find(gamepads, id, &GamepadState::id);
        return found == gamepads.end() ? nullptr : std::to_address(found);
    }

    struct RawInputCollector::Impl {
        std::array<RawInputSnapshot, 2> snapshots;
        std::size_t write{0};
        std::uint64_t nextGeneration{1};
    };

    RawInputCollector::RawInputCollector() : impl_(std::make_unique<Impl>()) {
        for (RawInputSnapshot &snapshot : impl_->snapshots) {
            snapshot.gamepads.reserve(4);
            snapshot.text.reserve(64);
        }
    }

    RawInputCollector::~RawInputCollector() = default;

    void RawInputCollector::BeginFrame(const FrameNumber frame) {
        RawInputSnapshot &next = impl_->snapshots[impl_->write];
        const RawInputSnapshot &previous = impl_->snapshots[1 - impl_->write];
        next = previous;
        next.frame = frame;
        next.text.clear();
        next.pointer.deltaX = 0.0F;
        next.pointer.deltaY = 0.0F;
        next.pointer.wheelX = 0.0F;
        next.pointer.wheelY = 0.0F;
        for (ButtonState &state : next.keyboard)
            Advance(state);
        for (ButtonState &state : next.pointer.buttons)
            Advance(state);
        for (GamepadState &pad : next.gamepads) {
            for (ButtonState &state : pad.buttons)
                Advance(state);
            for (ButtonState &state : pad.rawButtons)
                Advance(state);
        }
    }

    void RawInputCollector::SetKey(const Key key, const bool down) {
        auto &keys = impl_->snapshots[impl_->write].keyboard;
        if (Index(key) < keys.size())
            Set(keys[Index(key)], down);
    }

    void RawInputCollector::SetPointerButton(const PointerButton button, const bool down) {
        auto &buttons = impl_->snapshots[impl_->write].pointer.buttons;
        if (Index(button) < buttons.size())
            Set(buttons[Index(button)], down);
    }

    void RawInputCollector::SetPointerPosition(const float x, const float y) {
        PointerState &pointer = impl_->snapshots[impl_->write].pointer;
        pointer.deltaX += x - pointer.x;
        pointer.deltaY += y - pointer.y;
        pointer.x = x;
        pointer.y = y;
    }

    void RawInputCollector::AddPointerWheel(const float x, const float y) {
        PointerState &pointer = impl_->snapshots[impl_->write].pointer;
        pointer.wheelX += x;
        pointer.wheelY += y;
    }

    void RawInputCollector::AppendText(const std::string_view utf8) {
        impl_->snapshots[impl_->write].text.append(utf8);
    }

    void RawInputCollector::SetTextComposition(const std::string_view utf8, const std::int32_t selectionStart,
                                               const std::int32_t selectionLength) {
        TextCompositionState &composition = impl_->snapshots[impl_->write].composition;
        composition.text.assign(utf8);
        composition.selectionStart = std::max(selectionStart, 0);
        composition.selectionLength = std::max(selectionLength, 0);
        composition.active = !composition.text.empty();
    }

    void RawInputCollector::SetModifiers(const ModifierState modifiers) noexcept {
        impl_->snapshots[impl_->write].modifiers = modifiers;
    }

    void RawInputCollector::SetWindowState(const WindowInputState state) noexcept {
        impl_->snapshots[impl_->write].window = state;
    }

    void RawInputCollector::Neutralize() noexcept {
        RawInputSnapshot &snapshot = impl_->snapshots[impl_->write];
        for (ButtonState &state : snapshot.keyboard)
            Set(state, false);
        for (ButtonState &state : snapshot.pointer.buttons)
            Set(state, false);
        snapshot.modifiers = {};
        for (GamepadState &gamepad : snapshot.gamepads) {
            for (ButtonState &state : gamepad.buttons)
                Set(state, false);
            for (ButtonState &state : gamepad.rawButtons)
                Set(state, false);
            gamepad.axes.fill(0.0F);
            std::ranges::fill(gamepad.rawAxes, 0.0F);
        }
    }

    GamepadDeviceId RawInputCollector::ConnectGamepad(std::string name, const bool canonicalMapping, const std::size_t rawButtonCount,
                                                      const std::size_t rawAxisCount) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        std::uint32_t slot = 0;
        while (std::ranges::any_of(pads, [slot](const GamepadState &pad) {
            return pad.id.slot == slot;
        }))
            ++slot;
        const GamepadDeviceId id{slot, impl_->nextGeneration++};
        GamepadState state{.id = id, .name = std::move(name), .canonicalMapping = canonicalMapping};
        state.rawButtons.resize(rawButtonCount);
        state.rawAxes.resize(rawAxisCount);
        pads.push_back(std::move(state));
        return id;
    }

    bool RawInputCollector::DisconnectGamepad(const GamepadDeviceId id) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        return std::erase_if(pads, [id](const GamepadState &pad) {
            return pad.id == id;
        }) != 0;
    }

    bool RawInputCollector::SetGamepadButton(const GamepadDeviceId id, const GamepadButton button, const bool down) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || Index(button) >= pad->buttons.size())
            return false;
        Set(pad->buttons[Index(button)], down);
        return true;
    }

    bool RawInputCollector::SetGamepadAxis(const GamepadDeviceId id, const GamepadAxis axis, const float value) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || Index(axis) >= pad->axes.size() || !std::isfinite(value))
            return false;
        pad->axes[Index(axis)] = std::clamp(value, -1.0F, 1.0F);
        return true;
    }

    bool RawInputCollector::SetRawGamepadButton(const GamepadDeviceId id, const std::size_t button, const bool down) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || button >= pad->rawButtons.size())
            return false;
        Set(pad->rawButtons[button], down);
        return true;
    }

    bool RawInputCollector::SetRawGamepadAxis(const GamepadDeviceId id, const std::size_t axis, const float value) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || axis >= pad->rawAxes.size() || !std::isfinite(value))
            return false;
        pad->rawAxes[axis] = std::clamp(value, -1.0F, 1.0F);
        return true;
    }

    const RawInputSnapshot &RawInputCollector::Commit() {
        const std::size_t committed = impl_->write;
        impl_->write = 1 - impl_->write;
        return impl_->snapshots[committed];
    }

    bool BindingValidationReport::IsValid() const noexcept {
        return std::ranges::none_of(diagnostics, &BindingDiagnostic::blocking);
    }

    namespace {
        /** @brief Transparent hasher so string-like lookups avoid constructing temporary strings. */
        struct StringViewHash {
            using is_transparent = void;

            std::size_t operator()(const std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };

        void ValidateBinding(BindingValidationReport &report, const ActionId &action, const InputBinding &binding,
                             std::string_view invalidDeadzoneMessage, std::string_view unsupportedControlMessage,
                             std::string_view reservedShortcutMessage) {
            using enum BindingDiagnosticCode;
            if (!std::isfinite(binding.deadzone) || binding.deadzone < 0.0F || binding.deadzone >= 1.0F)
                report.diagnostics.emplace_back(InvalidDeadzone, action, std::string(invalidDeadzoneMessage));
            if (binding.chordSize > binding.chord.size())
                report.diagnostics.emplace_back(AmbiguousChord, action, "Binding chord exceeds the supported bounded size.");
            if (!IsControlSupported(binding))
                report.diagnostics.emplace_back(UnsupportedControl, action, std::string(unsupportedControlMessage));
            if (IsReservedShortcut(binding))
                report.diagnostics.emplace_back(ReservedShortcut, action, std::string(reservedShortcutMessage));
            if (!std::isfinite(binding.scale) || !std::isfinite(binding.digitalThreshold) || binding.digitalThreshold < 0.0F ||
                binding.digitalThreshold > 1.0F || binding.component > 1)
                report.diagnostics.emplace_back(UnsupportedControl, action, "Binding scale, threshold, or component is invalid.");
        }

        template <typename Bindings, typename MessageFn>
        void ReportDuplicateTransitions(BindingValidationReport &report, const ActionId &action, const Bindings &bindings,
                                        MessageFn &&makeMessage) {
            std::optional<std::string> message;
            for (std::size_t i = 0; i + 1 < bindings.size(); ++i) {
                for (std::size_t j = i + 1; j < bindings.size(); ++j) {
                    if (!SameTransition(bindings[i], bindings[j]))
                        continue;
                    if (!message)
                        message = std::forward<MessageFn>(makeMessage)();
                    report.diagnostics.emplace_back(BindingDiagnosticCode::DuplicateBinding, action, *message);
                }
            }
        }

        void ValidateOverrides(BindingValidationReport &report, const InputBindingProfile &profile,
                               const std::span<const ActionDescriptor> actions) {
            using enum BindingDiagnosticCode;
            std::unordered_set<std::string, StringViewHash, std::equal_to<>> overriddenActions;
            for (const BindingOverride &overrideValue : profile.overrides) {
                if (const auto action = std::ranges::find(actions, overrideValue.action, &ActionDescriptor::id); action == actions.end()) {
                    report.diagnostics.emplace_back(InvalidAction, overrideValue.action,
                                                    std::format("Binding override references unknown action '{}'.",
                                                                overrideValue.action.Value()));
                    continue;
                }
                if (!overriddenActions.insert(overrideValue.action.Value()).second)
                    report.diagnostics.emplace_back(DuplicateBinding, overrideValue.action,
                                                    std::format("The profile contains more than one override for action '{}'.",
                                                                overrideValue.action.Value()));
                for (const InputBinding &binding : overrideValue.bindings)
                    ValidateBinding(report, overrideValue.action, binding, "Binding deadzone must be in [0, 1).",
                                    "Binding references an unsupported control.", "Binding is reserved by the operating system.");
                ReportDuplicateTransitions(report, overrideValue.action, overrideValue.bindings, [&overrideValue] {
                    return std::format("Action '{}' override contains a duplicate binding on the same trigger.",
                                       overrideValue.action.Value());
                });
            }
        }

        struct EffectiveBinding {
            const ActionDescriptor *action;
            const InputBinding *binding;
        };

        void AppendEffectiveBindings(BindingValidationReport &report, std::vector<EffectiveBinding> &effective,
                                     const ActionDescriptor &action, const std::vector<InputBinding> &bindings) {
            using enum BindingDiagnosticCode;
            if (action.required && bindings.empty())
                report.diagnostics.emplace_back(RequiredActionUnbound, action.id,
                                                std::format("Required action '{}' has no binding in context '{}'.", action.id.Value(),
                                                            action.context.Value()));
            for (const InputBinding &binding : bindings) {
                ValidateBinding(report, action.id, binding, "Action binding deadzone must be in [0, 1).",
                                "Action references an unsupported control.", "Action uses an operating-system-reserved shortcut.");
                if ((action.valueType == ActionValueType::Digital || action.valueType == ActionValueType::Axis1D) && binding.component != 0)
                    report.diagnostics.emplace_back(DeviceExclusivityViolation, action.id,
                                                    std::format("Binding on action '{}' specifies component {}, but digital and 1D "
                                                                "actions require component 0.",
                                                                action.id.Value(), binding.component));
            }
            ReportDuplicateTransitions(report, action.id, bindings, [&action] {
                return std::format("Action '{}' contains a duplicate binding on the same trigger.", action.id.Value());
            });
            effective.reserve(effective.size() + bindings.size());
            for (const InputBinding &binding : bindings)
                effective.push_back({&action, &binding});
        }

        void ReportEffectiveBindingConflict(BindingValidationReport &report, const EffectiveBinding &left, const EffectiveBinding &right) {
            using enum BindingDiagnosticCode;
            if (left.action->id == right.action->id || left.action->context != right.action->context)
                return;

            const BindingControlKind kind = left.binding->kind;
            if (const bool analog = kind == BindingControlKind::GamepadAxis || kind == BindingControlKind::RawGamepadAxis ||
                                    kind == BindingControlKind::PointerWheelX || kind == BindingControlKind::PointerWheelY;
                analog) {
                if (!SameAnalogAxis(*left.binding, *right.binding))
                    return;
                report.diagnostics.emplace_back(DeviceExclusivityViolation, right.action->id,
                                                std::format("Action '{}' and action '{}' in context '{}' have an exclusive device "
                                                            "binding conflict on the same axis.",
                                                            right.action->id.Value(), left.action->id.Value(),
                                                            right.action->context.Value()));
                return;
            }
            if (SameTransition(*left.binding, *right.binding)) {
                report.diagnostics.emplace_back(DuplicateBinding, right.action->id,
                                                std::format("Action '{}' conflicts with action '{}' in context '{}' on the same trigger.",
                                                            right.action->id.Value(), left.action->id.Value(),
                                                            right.action->context.Value()));
                return;
            }
            if (ChordsOverlap(*left.binding, *right.binding))
                report.diagnostics.emplace_back(AmbiguousChord, right.action->id,
                                                std::format("Action '{}' chord overlaps action '{}' in context '{}'.",
                                                            right.action->id.Value(), left.action->id.Value(),
                                                            right.action->context.Value()));
        }

        void ValidateEffectiveBindings(BindingValidationReport &report, const InputBindingProfile &profile,
                                       const std::span<const ActionDescriptor> actions) {
            using enum BindingDiagnosticCode;
            std::vector<EffectiveBinding> effective;
            std::unordered_set<std::string, StringViewHash, std::equal_to<>> actionIds;
            for (const ActionDescriptor &action : actions) {
                if (!action.id.IsValid()) {
                    report.diagnostics.emplace_back(InvalidAction, action.id, "Action descriptor contains an empty action ID.");
                    continue;
                }
                if (!actionIds.insert(action.id.Value()).second) {
                    report.diagnostics.emplace_back(InvalidAction, action.id,
                                                    std::format("Action map contains more than one descriptor for action ID '{}'.",
                                                                action.id.Value()));
                    continue;
                }
                if (!action.context.IsValid()) {
                    report.diagnostics.emplace_back(AmbiguousContext, action.id,
                                                    std::format("Action '{}' has an invalid or empty context ID.", action.id.Value()));
                    continue;
                }

                const auto overrideValue = std::ranges::find(profile.overrides, action.id, &BindingOverride::action);
                const std::vector<InputBinding> &bindings =
                    overrideValue == profile.overrides.end() ? action.defaultBindings : overrideValue->bindings;
                AppendEffectiveBindings(report, effective, action, bindings);
            }
            for (std::size_t i = 0; i < effective.size(); ++i)
                for (std::size_t j = i + 1; j < effective.size(); ++j)
                    ReportEffectiveBindingConflict(report, effective[i], effective[j]);
        }
    }  // namespace

    BindingValidationReport ValidateBindingProfile(const std::span<const ActionDescriptor> actions, const InputBindingProfile &profile) {
        BindingValidationReport report;
        if (profile.schemaVersion != 1)
            report.diagnostics.emplace_back(BindingDiagnosticCode::InvalidSchema, ActionId{}, "Unsupported input profile schema.");
        ValidateOverrides(report, profile, actions);
        ValidateEffectiveBindings(report, profile, actions);
        return report;
    }

    Result<InputBindingProfile> MergeBindingProfiles(const InputBindingProfile &lower, const InputBindingProfile &higher) {
        if (lower.schemaVersion != 1 || higher.schemaVersion != 1)
            return Result<InputBindingProfile>::Failure(
                MakeError(Errors::ProfileInvalidSchema, "Cannot merge profiles with an unsupported schema."));
        InputBindingProfile merged = lower;
        merged.profileId = higher.profileId.empty() ? lower.profileId : higher.profileId;
        for (const BindingOverride &overrideValue : higher.overrides) {
            const auto existing = std::ranges::find(merged.overrides, overrideValue.action, &BindingOverride::action);
            if (existing == merged.overrides.end())
                merged.overrides.push_back(overrideValue);
            else
                *existing = overrideValue;
        }
        return Result<InputBindingProfile>::Success(std::move(merged));
    }

    namespace {
        InputBinding BindingFromJson(const nlohmann::json &json);

        /** @brief Parses the overrides array of a profile document, enforcing per-entry shape limits. */
        Result<std::vector<BindingOverride>> ParseOverrides(const nlohmann::json &json) {
            constexpr std::size_t maximumOverrides = 4096;
            constexpr std::size_t maximumBindingsPerAction = 64;
            const auto &overrides = json.at("overrides");
            if (!overrides.is_array() || overrides.size() > maximumOverrides)
                return Result<std::vector<BindingOverride>>::Failure(
                    MakeError(Errors::ProfileMalformed, "Input profile override count is invalid."));
            std::vector<BindingOverride> parsed;
            parsed.reserve(overrides.size());
            for (const auto &entry : overrides) {
                BindingOverride overrideValue{.action = ActionId{entry.at("action").get<std::string>()}};
                const auto &bindings = entry.at("bindings");
                if (!bindings.is_array() || bindings.size() > maximumBindingsPerAction)
                    return Result<std::vector<BindingOverride>>::Failure(
                        MakeError(Errors::ProfileMalformed, "Input profile binding count is invalid."));
                for (const auto &binding : bindings)
                    overrideValue.bindings.push_back(BindingFromJson(binding));
                parsed.push_back(std::move(overrideValue));
            }
            return Result<std::vector<BindingOverride>>::Success(std::move(parsed));
        }
    }  // namespace

    namespace {
        void to_json(nlohmann::json &json, const InputBinding &binding) {
            json = {{"kind", static_cast<int>(binding.kind)},
                    {"key", static_cast<int>(binding.key)},
                    {"pointerButton", static_cast<int>(binding.pointerButton)},
                    {"gamepadButton", static_cast<int>(binding.gamepadButton)},
                    {"gamepadAxis", static_cast<int>(binding.gamepadAxis)},
                    {"rawControl", binding.rawControl},
                    {"scale", binding.scale},
                    {"component", binding.component},
                    {"deadzoneKind", static_cast<int>(binding.deadzoneKind)},
                    {"deadzone", binding.deadzone},
                    {"digitalThreshold", binding.digitalThreshold},
                    {"chordSize", binding.chordSize},
                    {"modifiers",
                     {{"control", binding.requiredModifiers.control},
                      {"shift", binding.requiredModifiers.shift},
                      {"alt", binding.requiredModifiers.alt},
                      {"command", binding.requiredModifiers.command}}}};
            std::vector<int> chord;
            for (std::size_t i = 0; i < binding.chordSize; ++i)
                chord.push_back(static_cast<int>(binding.chord[i]));
            json["chord"] = chord;
        }

        InputBinding BindingFromJson(const nlohmann::json &json) {
            InputBinding binding;
            binding.kind = static_cast<BindingControlKind>(json.at("kind").get<int>());
            binding.key = static_cast<Key>(json.value("key", 0));
            binding.pointerButton = static_cast<PointerButton>(json.value("pointerButton", 0));
            binding.gamepadButton = static_cast<GamepadButton>(json.value("gamepadButton", 0));
            binding.gamepadAxis = static_cast<GamepadAxis>(json.value("gamepadAxis", 0));
            binding.rawControl = static_cast<std::uint16_t>(json.value("rawControl", 0));
            binding.scale = json.value("scale", 1.0F);
            binding.component = static_cast<std::uint8_t>(json.value("component", 0));
            binding.deadzoneKind = static_cast<DeadzoneKind>(json.value("deadzoneKind", 0));
            binding.deadzone = json.value("deadzone", 0.0F);
            binding.digitalThreshold = json.value("digitalThreshold", 0.5F);
            if (const auto chord = json.find("chord"); chord != json.end() && chord->is_array()) {
                binding.chordSize = static_cast<std::uint8_t>(std::min(chord->size(), binding.chord.size()));
                for (std::size_t i = 0; i < binding.chordSize; ++i)
                    binding.chord[i] = static_cast<Key>((*chord)[i].get<int>());
            }
            if (const auto mods = json.find("modifiers"); mods != json.end() && mods->is_object())
                binding.requiredModifiers = {mods->value("control", false), mods->value("shift", false), mods->value("alt", false),
                                             mods->value("command", false)};
            return binding;
        }
    }  // namespace

    Result<InputBindingProfile> ParseBindingProfile(const std::string_view value) {
        if (constexpr std::size_t maximumProfileBytes = 1024U * 1024U; value.empty() || value.size() > maximumProfileBytes)
            return Result<InputBindingProfile>::Failure(
                MakeError(Errors::ProfileMalformed, "Input profile size is outside the supported bounds."));
        try {
            bool duplicateKey = false;
            std::vector<std::unordered_set<std::string>> keysByDepth;
            const nlohmann::json::parser_callback_t callback = [&duplicateKey, &keysByDepth](const int depth,
                                                                                             const nlohmann::json::parse_event_t event,
                                                                                             nlohmann::json &parsed)  // NOSONAR
            {
                if (event == nlohmann::json::parse_event_t::object_start) {
                    if (keysByDepth.size() <= static_cast<std::size_t>(depth))
                        keysByDepth.resize(static_cast<std::size_t>(depth) + 1);
                    keysByDepth[static_cast<std::size_t>(depth)].clear();
                } else if (event == nlohmann::json::parse_event_t::key) {
                    const std::size_t objectDepth = depth > 0 ? static_cast<std::size_t>(depth - 1) : 0;
                    if (keysByDepth.size() <= objectDepth)
                        keysByDepth.resize(objectDepth + 1);
                    duplicateKey = !keysByDepth[objectDepth].insert(parsed.get<std::string>()).second || duplicateKey;
                }
                return !duplicateKey;
            };
            const nlohmann::json json = nlohmann::json::parse(value, callback, true, false);
            if (duplicateKey || !json.is_object())
                return Result<InputBindingProfile>::Failure(
                    MakeError(Errors::ProfileMalformed, "Input profile contains duplicate keys or is not an object."));
            InputBindingProfile profile;
            profile.schemaVersion = json.at("schemaVersion").get<std::uint32_t>();
            profile.profileId = json.at("profileId").get<std::string>();
            if (Result<std::vector<BindingOverride>> overrides = ParseOverrides(json); overrides.HasError())
                return Result<InputBindingProfile>::Failure(overrides.ErrorValue());
            else
                profile.overrides = std::move(overrides).Value();
            if (profile.schemaVersion != 1 || profile.profileId.empty() || profile.profileId.size() > 256)
                return Result<InputBindingProfile>::Failure(MakeError(Errors::ProfileInvalidSchema, "Invalid input profile schema."));
            return Result<InputBindingProfile>::Success(std::move(profile));
        } catch (const nlohmann::json::exception &exception) {
            return Result<InputBindingProfile>::Failure(MakeError(Errors::ProfileMalformed, exception.what()));
        }
    }

    Result<std::string> SerializeBindingProfile(const InputBindingProfile &profile) {
        if (profile.schemaVersion != 1 || profile.profileId.empty())
            return Result<std::string>::Failure(MakeError(Errors::ProfileInvalidSchema, "Invalid input profile schema."));
        nlohmann::json json{{"schemaVersion", profile.schemaVersion},
                            {"profileId", profile.profileId},
                            {"overrides", nlohmann::json::array()}};
        for (const BindingOverride &overrideValue : profile.overrides) {
            nlohmann::json entry{{"action", overrideValue.action.Value()}, {"bindings", nlohmann::json::array()}};
            for (const InputBinding &binding : overrideValue.bindings) {
                nlohmann::json bindingJson;
                to_json(bindingJson, binding);
                entry["bindings"].push_back(std::move(bindingJson));
            }
            json["overrides"].push_back(std::move(entry));
        }
        return Result<std::string>::Success(json.dump(2) + "\n");
    }

    Result<InputBindingProfile> LoadBindingProfile(const std::filesystem::path &path) {
        std::error_code error;
        if (const std::uintmax_t size = std::filesystem::file_size(path, error); error || size == 0 || size > 1024U * 1024U)
            return Result<InputBindingProfile>::Failure(
                MakeError(Errors::ProfileReadFailed, "Input profile is missing or exceeds the size limit."));
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return Result<InputBindingProfile>::Failure(MakeError(Errors::ProfileReadFailed, "Unable to read input profile."));
        return ParseBindingProfile(std::string(std::istreambuf_iterator<char>(input), {}));
    }

    Result<void> SaveBindingProfileAtomically(const std::filesystem::path &path, const InputBindingProfile &profile) {
        if (path.empty())
            return Result<void>::Failure(MakeError(Errors::ProfileWriteFailed, "Input profile path is empty."));
        const Result<std::string> serialized = SerializeBindingProfile(profile);
        if (serialized.HasError())
            return Result<void>::Failure(serialized.ErrorValue());
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return Result<void>::Failure(MakeError(Errors::ProfileDirectoryCreationFailed, error.message()));
        std::filesystem::path temporary = path;
        temporary += std::filesystem::path{".tmp."};
        temporary += std::filesystem::path{std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())};
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << serialized.Value();
            if (!output)
                return Result<void>::Failure(MakeError(Errors::ProfileWriteFailed, "Unable to write input profile."));
        }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD nativeError = GetLastError();
            std::filesystem::remove(temporary, error);
            return Result<void>::Failure(MakeError(Errors::ProfilePromotionFailed, "Atomic profile replacement failed with Win32 error " +
                                                                                       std::to_string(nativeError) + '.'));
        }
#else
        std::filesystem::rename(temporary, path, error);  // NOSONAR(cpp:S2083)
        if (error) {
            const std::string message = error.message();
            std::filesystem::remove(temporary, error);  // NOSONAR(cpp:S2083)
            return Result<void>::Failure(MakeError(Errors::ProfilePromotionFailed, message));
        }
#endif
        return Result<void>::Success();
    }

    struct InputRouter::Impl {
        struct Context {
            std::uint64_t token;
            InputContextId id;
            InputContextKind kind;
        };

        struct Capture {
            std::uint64_t token;
            std::uint64_t context;
            PointerButton button;
            IInputCaptureOwner *owner;
        };

        const RawInputSnapshot *snapshot{nullptr};
        const RawInputSnapshot *previousSnapshot{nullptr};
        RawInputSnapshot empty;
        std::vector<Context> contexts;
        std::optional<Capture> capture;
        std::vector<ActionDescriptor> actions;
        InputBindingProfile profile;
        std::unordered_set<std::uint32_t> consumedKeys;
        std::unordered_set<std::uint32_t> consumedPointerButtons;
        std::unordered_set<std::uint64_t> consumedGamepadTransitions;
        bool consumedWheelX{false};
        bool consumedWheelY{false};
        InputDeviceAssignments assignments;
        std::uint64_t nextToken{1};
    };

    namespace {
        int Priority(const InputContextKind kind) noexcept {
            return static_cast<int>(kind);
        }

        bool ModifiersMatch(const ModifierState &actual, const ModifierState &required) noexcept {
            return actual == required;
        }
    }  // namespace

    InputRouter::InputRouter() : impl_(std::make_unique<Impl>()) {}

    InputRouter::~InputRouter() = default;

    void InputRouter::BeginFrame(const RawInputSnapshot &snapshot) {
        impl_->previousSnapshot = impl_->snapshot;
        impl_->snapshot = &snapshot;
        impl_->consumedKeys.clear();
        impl_->consumedPointerButtons.clear();
        impl_->consumedGamepadTransitions.clear();
        impl_->consumedWheelX = false;
        impl_->consumedWheelY = false;
        impl_->assignments.RetainConnected(snapshot.gamepads);
        if (impl_->capture && !snapshot.window.focused)
            CancelCapture(CaptureCancellationReason::FocusLost);
        else if (impl_->capture && !snapshot.window.pointerDeviceAvailable)
            CancelCapture(CaptureCancellationReason::DeviceDisconnected);
        else if (impl_->capture && snapshot.State(Key::Escape).pressed)
            CancelCapture(CaptureCancellationReason::Escape);
    }

    InputContextToken InputRouter::PushContext(InputContextId id, const InputContextKind kind) {
        using enum InputContextKind;
        const std::uint64_t token = impl_->nextToken++;
        if (kind == ModalRoot || kind == ModalChild || kind == NativeDialog)
            CancelCapture(CaptureCancellationReason::ModalOpened);
        impl_->contexts.emplace_back(token, std::move(id), kind);
        return InputContextToken(this, token);
    }

    Result<PointerCaptureToken> InputRouter::CapturePointer(const InputContextToken &context, const PointerButton button,
                                                            IInputCaptureOwner &owner) {
        if (!IsContextActive(context))
            return Result<PointerCaptureToken>::Failure(MakeError(Errors::CaptureInactiveContext, "Input context is not active."));
        if (impl_->capture)
            return Result<PointerCaptureToken>::Failure(MakeError(Errors::CaptureBusy, "Pointer is already captured."));
        const std::uint64_t token = impl_->nextToken++;
        impl_->capture = Impl::Capture{token, context.token_, button, &owner};
        return Result<PointerCaptureToken>::Success(PointerCaptureToken(this, token));
    }

    void InputRouter::CancelCapture(const CaptureCancellationReason reason) noexcept {
        if (!impl_->capture)
            return;
        IInputCaptureOwner *owner = impl_->capture->owner;
        impl_->capture.reset();
        if (owner)
            owner->OnInputCaptureCancelled(reason);
    }

    bool InputRouter::HasCapture() const noexcept {
        return impl_->capture.has_value();
    }

    bool InputRouter::HasHigherPriorityContext(const InputContextKind kind) const noexcept {
        return std::ranges::any_of(impl_->contexts, [kind](const Impl::Context &context) {
            return Priority(context.kind) > Priority(kind);
        });
    }

    bool InputRouter::IsContextActive(const InputContextToken &context) const noexcept {
        if (context.router_ != this || !TokenActive(context.token_))
            return false;
        const auto found = std::ranges::find(impl_->contexts, context.token_, &Impl::Context::token);
        return found != impl_->contexts.end() && std::ranges::none_of(impl_->contexts, [&](const Impl::Context &candidate) {
            return Priority(candidate.kind) > Priority(found->kind) || (candidate.kind == found->kind && candidate.token > found->token);
        });
    }

    const RawInputSnapshot &InputRouter::Snapshot() const noexcept {
        return impl_->snapshot ? *impl_->snapshot : impl_->empty;
    }

    Result<void> InputRouter::SetActionMap(std::vector<ActionDescriptor> actions, InputBindingProfile profile) {
        if (const BindingValidationReport validation = ValidateBindingProfile(actions, profile); !validation.IsValid())
            return Result<void>::Failure(MakeError(Errors::ActionMapValidationFailed, validation.diagnostics.front().message));
        impl_->actions = std::move(actions);
        impl_->profile = std::move(profile);
        return Result<void>::Success();
    }

    std::span<const ActionDescriptor> InputRouter::Actions() const noexcept {
        return impl_->actions;
    }

    const InputBindingProfile &InputRouter::Profile() const noexcept {
        return impl_->profile;
    }

    Result<void> InputRouter::SetProfile(InputBindingProfile profile) {
        if (const BindingValidationReport validation = ValidateBindingProfile(impl_->actions, profile); !validation.IsValid())
            return Result<void>::Failure(MakeError(Errors::ProfileValidationFailed, validation.diagnostics.front().message));
        impl_->profile = std::move(profile);
        return Result<void>::Success();
    }

    namespace {
        template <typename ImplType>
        [[nodiscard]] std::pair<float, ButtonState> EvaluateControlBinding(const InputBinding &binding, const RawInputSnapshot &snapshot,
                                                                           const std::optional<PlayerId> player, ImplType &impl) {
            float axis = 0.0F;
            ButtonState state;
            switch (binding.kind) {
                case BindingControlKind::Key:
                    state = snapshot.State(binding.key);
                    if (state.pressed && !impl.consumedKeys.insert(static_cast<std::uint32_t>(binding.key)).second)
                        state.pressed = false;
                    axis = state.down ? 1.0F : 0.0F;
                    break;
                case BindingControlKind::PointerButton:
                    state = snapshot.State(binding.pointerButton);
                    if (state.pressed && !impl.consumedPointerButtons.insert(static_cast<std::uint32_t>(binding.pointerButton)).second)
                        state.pressed = false;
                    axis = state.down ? 1.0F : 0.0F;
                    break;
                case BindingControlKind::PointerWheelX:
                    axis = impl.consumedWheelX ? 0.0F : snapshot.pointer.wheelX;
                    state.pressed = axis != 0.0F;
                    state.down = state.pressed;
                    impl.consumedWheelX = impl.consumedWheelX || state.pressed;
                    break;
                case BindingControlKind::PointerWheelY:
                    axis = impl.consumedWheelY ? 0.0F : snapshot.pointer.wheelY;
                    state.pressed = axis != 0.0F;
                    state.down = state.pressed;
                    impl.consumedWheelY = impl.consumedWheelY || state.pressed;
                    break;
                default: {
                    const auto result = EvaluateGamepadBinding(binding, snapshot, impl.previousSnapshot, impl.assignments, player,
                                                               impl.consumedGamepadTransitions);
                    axis = result.axis;
                    state = result.state;
                    break;
                }
            }
            return {axis, state};
        }

        void ApplyRadialDeadzone(ActionValue &value, const float radialDeadzone) {
            const float magnitude = std::sqrt(value.x * value.x + value.y * value.y);
            if (magnitude <= radialDeadzone || magnitude <= std::numeric_limits<float>::epsilon()) {
                value.y = 0.0F;
                value.x = 0.0F;
            } else {
                const float remapped = std::min(1.0F, (magnitude - radialDeadzone) / (1.0F - radialDeadzone));
                value.x = value.x / magnitude * remapped;
                value.y = value.y / magnitude * remapped;
            }
        }

        [[nodiscard]] bool IsBindingActive(const InputBinding &binding, const RawInputSnapshot &snapshot) {
            if (!ModifiersMatch(snapshot.modifiers, binding.requiredModifiers))
                return false;
            for (std::size_t index = 0; index < binding.chordSize; ++index)
                if (!snapshot.State(binding.chord[index]).down)
                    return false;
            return true;
        }

        struct BindingAccumulationTarget {
            ActionValue &value;
            bool &radial2D;
            float &radialDeadzone;
        };

        template <typename ImplType>
        void AccumulateBinding(const InputBinding &binding, const ActionDescriptor &descriptor, const RawInputSnapshot &snapshot,
                               const std::optional<PlayerId> player, ImplType &impl, BindingAccumulationTarget &target) {
            if (!IsBindingActive(binding, snapshot))
                return;

            auto [rawAxis, state] = EvaluateControlBinding(binding, snapshot, player, impl);
            const float axis = rawAxis * binding.scale;
            target.radial2D =
                target.radial2D || (descriptor.valueType == ActionValueType::Axis2D && binding.deadzoneKind == DeadzoneKind::Radial);
            target.radialDeadzone = std::max(target.radialDeadzone, binding.deadzone);
            if (descriptor.valueType == ActionValueType::Axis2D) {
                if (binding.component == 0)
                    target.value.x += axis;
                else
                    target.value.y += axis;
            } else
                target.value.x += axis;
            target.value.down = target.value.down || state.down;
            target.value.pressed = target.value.pressed || state.pressed;
            target.value.released = target.value.released || state.released;
        }
    }  // namespace

    ActionValue InputRouter::ReadAction(const InputContextToken &context, const ActionId &actionId, const std::optional<PlayerId> player) {
        ActionValue value;
        if (!IsContextActive(context))
            return value;
        const auto descriptor = std::ranges::find(impl_->actions, actionId, &ActionDescriptor::id);
        if (descriptor == impl_->actions.end())
            return value;
        if (const auto contextEntry = std::ranges::find(impl_->contexts, context.token_, &Impl::Context::token);
            contextEntry == impl_->contexts.end() || contextEntry->id != descriptor->context)
            return value;
        const std::vector<InputBinding> *bindings = &descriptor->defaultBindings;
        if (const auto overrideValue = std::ranges::find(impl_->profile.overrides, actionId, &BindingOverride::action);
            overrideValue != impl_->profile.overrides.end())
            bindings = &overrideValue->bindings;
        const RawInputSnapshot &snapshot = Snapshot();
        bool radial2D = false;
        float radialDeadzone = 0.0F;
        BindingAccumulationTarget target{.value = value, .radial2D = radial2D, .radialDeadzone = radialDeadzone};
        for (const InputBinding &binding : *bindings)
            AccumulateBinding(binding, *descriptor, snapshot, player, *impl_, target);
        value.x = std::clamp(value.x, -1.0F, 1.0F);
        value.y = std::clamp(value.y, -1.0F, 1.0F);
        if (radial2D)
            ApplyRadialDeadzone(value, radialDeadzone);
        return value;
    }

    bool InputRouter::ConsumeKey(const InputContextToken &context, const Key key) {
        if (!IsContextActive(context) || !Snapshot().State(key).pressed)
            return false;
        return impl_->consumedKeys.insert(static_cast<std::uint32_t>(key)).second;
    }

    bool InputRouter::ConsumePointerButton(const InputContextToken &context, const PointerButton button) {
        if (!IsContextActive(context) || !Snapshot().State(button).pressed)
            return false;
        return impl_->consumedPointerButtons.insert(static_cast<std::uint32_t>(button)).second;
    }

    bool InputRouter::AssignGamepad(const PlayerId player, const GamepadDeviceId gamepad) {
        return Snapshot().FindGamepad(gamepad) != nullptr && impl_->assignments.Assign(player, gamepad);
    }

    void InputRouter::UnassignGamepad(const GamepadDeviceId gamepad) noexcept {
        impl_->assignments.Unassign(gamepad);
    }

    std::optional<PlayerId> InputRouter::PlayerForGamepad(const GamepadDeviceId gamepad) const noexcept {
        return impl_->assignments.PlayerFor(gamepad);
    }

    void InputRouter::RemoveContext(const std::uint64_t token) noexcept {
        if (impl_->capture && impl_->capture->context == token)
            CancelCapture(CaptureCancellationReason::ContextRemoved);
        std::erase_if(impl_->contexts, [token](const Impl::Context &context) {
            return context.token == token;
        });
    }

    void InputRouter::ReleaseCapture(const std::uint64_t token) noexcept {
        if (impl_->capture && impl_->capture->token == token)
            impl_->capture.reset();
    }

    bool InputRouter::TokenActive(const std::uint64_t token) const noexcept {
        return std::ranges::any_of(impl_->contexts, [token](const Impl::Context &context) {
            return context.token == token;
        });
    }

    bool InputRouter::CaptureActive(const std::uint64_t token) const noexcept {
        return impl_->capture && impl_->capture->token == token;
    }

    InputContextToken::~InputContextToken() {
        Reset();
    }

    InputContextToken::InputContextToken(InputContextToken &&other) noexcept
        : router_(std::exchange(other.router_, nullptr)), token_(std::exchange(other.token_, 0)) {}

    InputContextToken &InputContextToken::operator=(InputContextToken &&other) noexcept {
        if (this != &other) {
            Reset();
            router_ = std::exchange(other.router_, nullptr);
            token_ = std::exchange(other.token_, 0);
        }
        return *this;
    }

    void InputContextToken::Reset() noexcept {
        if (router_)
            router_->RemoveContext(token_);
        router_ = nullptr;
        token_ = 0;
    }

    bool InputContextToken::IsActive() const noexcept {
        return router_ && router_->TokenActive(token_);
    }

    PointerCaptureToken::~PointerCaptureToken() {
        Release();
    }

    PointerCaptureToken::PointerCaptureToken(PointerCaptureToken &&other) noexcept
        : router_(std::exchange(other.router_, nullptr)), token_(std::exchange(other.token_, 0)) {}

    PointerCaptureToken &PointerCaptureToken::operator=(PointerCaptureToken &&other) noexcept {
        if (this != &other) {
            Release();
            router_ = std::exchange(other.router_, nullptr);
            token_ = std::exchange(other.token_, 0);
        }
        return *this;
    }

    void PointerCaptureToken::Release() noexcept {
        if (router_)
            router_->ReleaseCapture(token_);
        router_ = nullptr;
        token_ = 0;
    }

    bool PointerCaptureToken::IsActive() const noexcept {
        return router_ && router_->CaptureActive(token_);
    }

    void InputService::BeginFrame(const FrameNumber frame) {
        collector_.BeginFrame(frame);
    }

    RawInputCollector &InputService::Collector() noexcept {
        return collector_;
    }

    InputRouter &InputService::Router() noexcept {
        return router_;
    }

    const RawInputSnapshot &InputService::CommitFrame() {
        const RawInputSnapshot &snapshot = collector_.Commit();
        router_.BeginFrame(snapshot);
        return snapshot;
    }

    /** @copydoc GameplayInputFrameBuilder::GameplayInputFrameBuilder */
    GameplayInputFrameBuilder::GameplayInputFrameBuilder(ActionId move, ActionId look, ActionId jump, ActionId interact)
        : move_(std::move(move)), look_(std::move(look)), jump_(std::move(jump)), interact_(std::move(interact)) {}

    /** @copydoc GameplayInputFrameBuilder::Capture */
    void GameplayInputFrameBuilder::Capture(InputRouter &router, const InputContextToken &context, const std::optional<PlayerId> player) {
        const RawInputSnapshot &snapshot = router.Snapshot();
        if (hasCapturedFrame_ && capturedFrame_ == snapshot.frame)
            return;
        capturedFrame_ = snapshot.frame;
        hasCapturedFrame_ = true;
        if (!snapshot.window.focused || !router.IsContextActive(context)) {
            moveX_ = moveY_ = lookX_ = lookY_ = 0.0F;
            pendingJump_ = pendingInteract_ = false;
            moveDown_ = pendingMovePressed_ = pendingMoveReleased_ = false;
            return;
        }
        const ActionValue move = router.ReadAction(context, move_, player);
        const ActionValue look = router.ReadAction(context, look_, player);
        const ActionValue jump = router.ReadAction(context, jump_, player);
        const ActionValue interact = router.ReadAction(context, interact_, player);
        moveX_ = move.x;
        moveY_ = move.y;
        lookX_ = look.x;
        lookY_ = look.y;
        pendingJump_ = pendingJump_ || jump.pressed;
        pendingInteract_ = pendingInteract_ || interact.pressed;
        moveDown_ = move.down;
        pendingMovePressed_ = pendingMovePressed_ || move.pressed;
        pendingMoveReleased_ = pendingMoveReleased_ || move.released;
    }

    /** @copydoc GameplayInputFrameBuilder::Consume */
    GameplayInputFrame GameplayInputFrameBuilder::Consume(const SimulationTick tick) noexcept {
        return GameplayInputFrame{tick,
                                  moveX_,
                                  moveY_,
                                  lookX_,
                                  lookY_,
                                  std::exchange(pendingJump_, false),
                                  std::exchange(pendingInteract_, false),
                                  moveDown_,
                                  std::exchange(pendingMovePressed_, false),
                                  std::exchange(pendingMoveReleased_, false)};
    }

    /** @copydoc GameplayInputFrameBuilder::Reset */
    void GameplayInputFrameBuilder::Reset() noexcept {
        hasCapturedFrame_ = false;
        moveX_ = moveY_ = lookX_ = lookY_ = 0.0F;
        pendingJump_ = pendingInteract_ = false;
        moveDown_ = pendingMovePressed_ = pendingMoveReleased_ = false;
    }

    void GameplayInputRecording::Record(const GameplayInputFrame &frame) {
        frames_.push_back(frame);
    }

    void GameplayInputRecording::ResetReplay() noexcept {
        replayIndex_ = 0;
    }

    std::optional<GameplayInputFrame> GameplayInputRecording::Next() {
        return replayIndex_ < frames_.size() ? std::optional{frames_[replayIndex_++]} : std::nullopt;
    }

    bool InputDeviceAssignments::Assign(const PlayerId player, const GamepadDeviceId gamepad) {
        if (!gamepad.IsValid())
            return false;
        Unassign(gamepad);
        std::erase_if(assignments_, [player](const auto &entry) {
            return entry.second == player;
        });
        assignments_.emplace_back(gamepad, player);
        return true;
    }

    void InputDeviceAssignments::Unassign(const GamepadDeviceId gamepad) noexcept {
        std::erase_if(assignments_, [gamepad](const auto &entry) {
            return entry.first == gamepad;
        });
    }

    void InputDeviceAssignments::RetainConnected(const std::span<const GamepadState> gamepads) noexcept {
        std::erase_if(assignments_, [gamepads](const auto &entry) {
            return std::ranges::none_of(gamepads, [&entry](const GamepadState &gamepad) {
                return gamepad.id == entry.first;
            });
        });
    }

    std::optional<PlayerId> InputDeviceAssignments::PlayerFor(const GamepadDeviceId gamepad) const noexcept {
        const auto found = std::ranges::find(assignments_, gamepad, &std::pair<GamepadDeviceId, PlayerId>::first);
        return found == assignments_.end() ? std::nullopt : std::optional{found->second};
    }

    VirtualGamepad::~VirtualGamepad() {
        Disconnect();
    }

    VirtualGamepad::VirtualGamepad(VirtualGamepad &&other) noexcept
        : collector_(std::exchange(other.collector_, nullptr)), id_(std::exchange(other.id_, {})) {}

    VirtualGamepad &VirtualGamepad::operator=(VirtualGamepad &&other) noexcept {
        if (this != &other) {
            Disconnect();
            collector_ = std::exchange(other.collector_, nullptr);
            id_ = std::exchange(other.id_, {});
        }
        return *this;
    }

    GamepadDeviceId VirtualGamepad::Connect(std::string name) {
        Disconnect();
        if (collector_ != nullptr)
            id_ = collector_->ConnectGamepad(std::move(name));
        return id_;
    }

    void VirtualGamepad::Disconnect() {
        if (collector_ != nullptr && id_.IsValid())
            (void)collector_->DisconnectGamepad(id_);
        id_ = {};
    }

    bool VirtualGamepad::Press(const GamepadButton button) {
        return collector_ != nullptr && id_.IsValid() && collector_->SetGamepadButton(id_, button, true);
    }

    bool VirtualGamepad::Release(const GamepadButton button) {
        return collector_ != nullptr && id_.IsValid() && collector_->SetGamepadButton(id_, button, false);
    }

    bool VirtualGamepad::SetAxis(const GamepadAxis axis, const float value) {
        return collector_ != nullptr && id_.IsValid() && collector_->SetGamepadAxis(id_, axis, value);
    }
}  // namespace Horo::Input
