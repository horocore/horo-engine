#include "Horo/Runtime/Input.h"

namespace Horo::Input {
    namespace {
        /** @brief Builds static numbered fallback labels for the bounded raw-control domain. */
        constexpr auto RawControlLabels(const std::string_view prefix) {
            std::array<std::array<char, 24>, 256> labels{};
            for (std::size_t index = 0; index < labels.size(); ++index) {
                auto &label = labels[index];
                std::size_t offset = 0;
                for (const char character : prefix)
                    label[offset++] = character;
                if (index >= 100)
                    label[offset++] = static_cast<char>('0' + index / 100);
                if (index >= 10)
                    label[offset++] = static_cast<char>('0' + index / 10 % 10);
                label[offset] = static_cast<char>('0' + index % 10);
            }
            return labels;
        }

        /** @brief Returns an allocation-free canonical table label or empty unsupported evidence. */
        template <std::size_t Size>
        std::string_view LabelAt(const std::array<std::string_view, Size> &labels, const std::uint16_t control) noexcept {
            return control < Size ? labels[control] : std::string_view{};
        }
    }  // namespace

    /** @copydoc CanonicalActionSource */
    ActionSource CanonicalActionSource(const InputBinding &binding, const std::optional<GamepadDeviceId> device) noexcept {
        using enum BindingControlKind;
        using enum InputModality;
        ActionSource source;
        source.glyph.kind = binding.kind;
        switch (binding.kind) {
            case Key:
                source.modality = KeyboardMouse;
                source.glyph.control = static_cast<std::uint16_t>(binding.key);
                break;
            case PointerButton:
                source.modality = KeyboardMouse;
                source.glyph.control = static_cast<std::uint16_t>(binding.pointerButton);
                break;
            case PointerWheelX:
            case PointerWheelY:
                source.modality = KeyboardMouse;
                break;
            case GamepadButton:
                source.modality = Gamepad;
                source.glyph.control = static_cast<std::uint16_t>(binding.gamepadButton);
                source.gamepad = device;
                break;
            case GamepadAxis:
                source.modality = Gamepad;
                source.glyph.control = static_cast<std::uint16_t>(binding.gamepadAxis);
                source.gamepad = device;
                break;
            case RawGamepadButton:
            case RawGamepadAxis:
                source.modality = Gamepad;
                source.gamepad = device;
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
                 "F2",  "F3",    "F4",        "F5",     "F6",   "F7",    "F8", "F9",   "F10",  "F11", "F12",     "=",         "-"};
        std::string_view label;
        static constexpr auto rawButtons = RawControlLabels("Raw button ");
        static constexpr auto rawAxes = RawControlLabels("Raw axis ");
        using enum BindingControlKind;
        switch (id.kind) {
            case Key:
                label = LabelAt(keys, id.control);
                break;
            case PointerButton:
                label = LabelAt(pointer, id.control);
                break;
            case GamepadButton:
                label = LabelAt(buttons, id.control);
                break;
            case GamepadAxis:
                label = LabelAt(axes, id.control);
                break;
            case RawGamepadButton:
                if (id.control < rawButtons.size())
                    label = rawButtons[id.control].data();
                break;
            case RawGamepadAxis:
                if (id.control < rawAxes.size())
                    label = rawAxes[id.control].data();
                break;
            case PointerWheelX:
                if (id.control == 0)
                    label = "Wheel X";
                break;
            case PointerWheelY:
                if (id.control == 0)
                    label = "Wheel Y";
                break;
            default:
                break;
        }
        return {id, label.empty() ? InputGlyphSupport::Unsupported : InputGlyphSupport::CanonicalLabel, label};
    }

    /** @copydoc ResolveInputGlyph */
    InputGlyphPresentation ResolveInputGlyph(const InputGlyphId id, const IInputGlyphProvider *provider,
                                             const std::optional<GamepadDeviceId> device) noexcept {
        const auto fallback = CanonicalGlyph(id);
        if (!provider || fallback.support == InputGlyphSupport::Unsupported)
            return fallback;
        const auto supplied = provider->Resolve(id, device);
        if (supplied.id != id || supplied.support != InputGlyphSupport::CanonicalLabel || supplied.label.empty())
            return fallback;
        return supplied;
    }
}  // namespace Horo::Input
