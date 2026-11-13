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

        constexpr std::array<std::string_view, static_cast<std::size_t>(GamepadButton::Count)>
            kButtonLabels{"South",       "East",  "West",   "North",    "Left shoulder", "Right shoulder", "Left stick",
                          "Right stick", "Start", "Select", "D-pad up", "D-pad down",    "D-pad left",     "D-pad right"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(GamepadAxis::Count)> kAxisLabels{"Left stick X",  "Left stick Y",
                                                                                                         "Right stick X", "Right stick Y",
                                                                                                         "Left trigger",  "Right trigger"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(PointerButton::Count)> kPointerLabels{"Primary", "Secondary",
                                                                                                              "Middle", "Auxiliary 1",
                                                                                                              "Auxiliary 2"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(Key::Count)>
            kKeyLabels{"",    "A",     "B",         "C",      "D",    "E",     "F",  "G",    "H",    "I",   "J",       "K",         "L",
                       "M",   "N",     "O",         "P",      "Q",    "R",     "S",  "T",    "U",    "V",   "W",       "X",         "Y",
                       "Z",   "0",     "1",         "2",      "3",    "4",     "5",  "6",    "7",    "8",   "9",       "Escape",    "Enter",
                       "Tab", "Space", "Backspace", "Delete", "Left", "Right", "Up", "Down", "Home", "End", "Page up", "Page down", "F1",
                       "F2",  "F3",    "F4",        "F5",     "F6",   "F7",    "F8", "F9",   "F10",  "F11", "F12",     "=",         "-"};
        constexpr auto kRawButtonLabels = RawControlLabels("Raw button ");
        constexpr auto kRawAxisLabels = RawControlLabels("Raw axis ");
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
            case GyroscopeAxis:
                source.modality = Motion;
                source.glyph.control =
                    static_cast<std::uint16_t>(binding.gyroscope.axis) + (binding.gyroscope.source == GyroscopeSource::Gamepad ? 3 : 0);
                if (binding.gyroscope.source == GyroscopeSource::Gamepad)
                    source.gamepad = device;
                break;
        }
        return source;
    }

    /** @copydoc CanonicalGlyph */
    InputGlyphPresentation CanonicalGlyph(const InputGlyphId id) noexcept {
        std::string_view label;
        using enum BindingControlKind;
        switch (id.kind) {
            case Key:
                label = LabelAt(kKeyLabels, id.control);
                break;
            case PointerButton:
                label = LabelAt(kPointerLabels, id.control);
                break;
            case GamepadButton:
                label = LabelAt(kButtonLabels, id.control);
                break;
            case GamepadAxis:
                label = LabelAt(kAxisLabels, id.control);
                break;
            case RawGamepadButton:
                if (id.control < kRawButtonLabels.size())
                    label = kRawButtonLabels[id.control].data();
                break;
            case RawGamepadAxis:
                if (id.control < kRawAxisLabels.size())
                    label = kRawAxisLabels[id.control].data();
                break;
            case GyroscopeAxis: {
                static constexpr std::array<std::string_view, 6> gyro{"Gyroscope X",
                                                                      "Gyroscope Y",
                                                                      "Gyroscope Z",
                                                                      "Controller gyroscope X",
                                                                      "Controller gyroscope Y",
                                                                      "Controller gyroscope Z"};
                label = LabelAt(gyro, id.control);
                break;
            }
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
