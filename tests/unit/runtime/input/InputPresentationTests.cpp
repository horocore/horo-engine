#include "Horo/Runtime/Input.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace Horo {
    namespace {
        TEST_CASE("Canonical glyph vocabulary has stable supported controls and typed missing capabilities", "[input][runtime_ui][glyph]") {
            using enum Input::BindingControlKind;
            const std::array kinds{Key, PointerButton, GamepadButton, GamepadAxis, RawGamepadButton, RawGamepadAxis};
            const std::array counts{static_cast<std::size_t>(Input::Key::Count),
                                    static_cast<std::size_t>(Input::PointerButton::Count),
                                    static_cast<std::size_t>(Input::GamepadButton::Count),
                                    static_cast<std::size_t>(Input::GamepadAxis::Count),
                                    std::size_t{256},
                                    std::size_t{256}};
            for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
                for (std::size_t control = kind == 0 ? 1 : 0; control < counts[kind]; ++control) {
                    const Input::InputGlyphId id{kinds[kind], static_cast<std::uint16_t>(control)};
                    const auto before = Tests::AllocationProbe::Count();
                    const auto glyph = Input::CanonicalGlyph(id);
                    const auto after = Tests::AllocationProbe::Count();
                    CHECK(before == after);
                    CHECK(glyph.id == id);
                    CHECK(glyph.support == Input::InputGlyphSupport::CanonicalLabel);
                    CHECK_FALSE(glyph.label.empty());
                }
                CHECK(Input::CanonicalGlyph({kinds[kind], 65535}).support == Input::InputGlyphSupport::Unsupported);
            }
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::PointerWheelX, 0}).label == "Wheel X");
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::PointerWheelY, 0}).label == "Wheel Y");
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::PointerWheelX, 1}).support == Input::InputGlyphSupport::Unsupported);
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::RawGamepadAxis, 1}).label == "Raw axis 1");
        }

        TEST_CASE("Optional glyph packages preserve control identity and fall back without allocations", "[input][runtime_ui][glyph]") {
            struct Provider final : Input::IInputGlyphProvider {
                Input::InputGlyphPresentation presentation;
                mutable std::optional<Input::GamepadDeviceId> observedDevice;

                Input::InputGlyphPresentation Resolve(const Input::InputGlyphId,
                                                      const std::optional<Input::GamepadDeviceId> device) const noexcept override {
                    observedDevice = device;
                    return presentation;
                }
            } provider;

            const Input::InputGlyphId id{Input::BindingControlKind::GamepadButton, 0};
            const Input::GamepadDeviceId device{2, 17};
            provider.presentation = {id, Input::InputGlyphSupport::CanonicalLabel, "Package South"};
            const auto before = Tests::AllocationProbe::Count();
            const auto supplied = Input::ResolveInputGlyph(id, &provider, device);
            const auto after = Tests::AllocationProbe::Count();
            CHECK(before == after);
            CHECK(supplied.id == id);
            CHECK(supplied.label == "Package South");
            CHECK(provider.observedDevice == device);
            provider.presentation.id.control = 1;
            CHECK(Input::ResolveInputGlyph(id, &provider, device).label == "South");
            provider.presentation = {id, Input::InputGlyphSupport::Unsupported, {}};
            CHECK(Input::ResolveInputGlyph(id, &provider, device).label == "South");
            CHECK(Input::ResolveInputGlyph(id).label == "South");
        }

    }  // namespace
}  // namespace Horo
