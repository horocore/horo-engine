#include "Horo/Runtime/Input.h"

namespace Horo::Input {
    /** @copydoc CanonicalActionSource */
    ActionSource CanonicalActionSource(const InputBinding &binding, const std::optional<GamepadDeviceId> device) noexcept {
        using enum BindingControlKind;
        ActionSource source;
        source.glyph.kind = binding.kind;
        switch (binding.kind) {
            case Key:
                source.modality = InputModality::KeyboardMouse;
                source.glyph.control = static_cast<std::uint16_t>(binding.key);
                break;
            case PointerButton:
                source.modality = InputModality::KeyboardMouse;
                source.glyph.control = static_cast<std::uint16_t>(binding.pointerButton);
                break;
            case PointerWheelX:
            case PointerWheelY:
                source.modality = InputModality::KeyboardMouse;
                break;
            case GamepadButton:
                source.modality = InputModality::Gamepad;
                source.glyph.control = static_cast<std::uint16_t>(binding.gamepadButton);
                source.gamepad = device;
                break;
            case GamepadAxis:
                source.modality = InputModality::Gamepad;
                source.glyph.control = static_cast<std::uint16_t>(binding.gamepadAxis);
                source.gamepad = device;
                break;
            case RawGamepadButton:
            case RawGamepadAxis:
                source.glyph.control = binding.rawControl;
                break;
        }
        return source;
    }

    /** @copydoc CanonicalGlyph */
    InputGlyphPresentation CanonicalGlyph(const InputGlyphId id) noexcept {
        static constexpr std::array<std::string_view, static_cast<std::size_t>(GamepadButton::Count)>
            buttons{"South",       "East",  "West",   "North",    "Left shoulder", "Right shoulder", "Left stick",
                    "Right stick", "Start", "Select", "D-pad up", "D-pad down",    "D-pad left",     "D-pad right"};
        static constexpr std::array<std::string_view, static_cast<std::size_t>(GamepadAxis::Count)> axes{"Left stick X",  "Left stick Y",
                                                                                                         "Right stick X", "Right stick Y",
                                                                                                         "Left trigger",  "Right trigger"};
        static constexpr std::array<std::string_view, static_cast<std::size_t>(PointerButton::Count)> pointer{"Primary", "Secondary",
                                                                                                              "Middle", "Auxiliary 1",
                                                                                                              "Auxiliary 2"};
        static constexpr std::array<std::string_view, static_cast<std::size_t>(Key::Count)>
            keys{"",    "A",     "B",         "C",      "D",    "E",     "F",  "G",    "H",    "I",   "J",       "K",         "L",
                 "M",   "N",     "O",         "P",      "Q",    "R",     "S",  "T",    "U",    "V",   "W",       "X",         "Y",
                 "Z",   "0",     "1",         "2",      "3",    "4",     "5",  "6",    "7",    "8",   "9",       "Escape",    "Enter",
                 "Tab", "Space", "Backspace", "Delete", "Left", "Right", "Up", "Down", "Home", "End", "Page up", "Page down", "F1",
                 "F2",  "F3",    "F4",        "F5",     "F6",   "F7",    "F8", "F9",   "F10",  "F11", "F12"};
        std::string_view label;
        switch (id.kind) {
            case BindingControlKind::Key:
                if (id.control < keys.size())
                    label = keys[id.control];
                break;
            case BindingControlKind::PointerButton:
                if (id.control < pointer.size())
                    label = pointer[id.control];
                break;
            case BindingControlKind::GamepadButton:
                if (id.control < buttons.size())
                    label = buttons[id.control];
                break;
            case BindingControlKind::GamepadAxis:
                if (id.control < axes.size())
                    label = axes[id.control];
                break;
            case BindingControlKind::PointerWheelX:
                if (id.control == 0)
                    label = "Wheel X";
                break;
            case BindingControlKind::PointerWheelY:
                if (id.control == 0)
                    label = "Wheel Y";
                break;
            default:
                break;
        }
        return {id, label.empty() ? InputGlyphSupport::Unsupported : InputGlyphSupport::CanonicalLabel, label};
    }
}  // namespace Horo::Input
