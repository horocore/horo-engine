#include "SdlInputBackend.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo::Input;

    struct NativeVirtualPad final {
        SDL_JoystickID native{};
        SDL_Joystick *joystick{};
        Uint16 low{};
        Uint16 high{};
        std::string name;

        explicit NativeVirtualPad(const bool canonical, const bool rumble = true) {
            static unsigned int incarnation{};
            name = "Horo qualification virtual device " + std::to_string(++incarnation);
            REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
            SDL_VirtualJoystickDesc descriptor{};
            SDL_INIT_INTERFACE(&descriptor);
            descriptor.type = canonical ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
            descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
            descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            descriptor.button_mask = (1U << SDL_GAMEPAD_BUTTON_SOUTH);
            descriptor.axis_mask = (1U << SDL_GAMEPAD_AXIS_LEFTX);
            descriptor.name = name.c_str();
            descriptor.userdata = this;
            if (rumble)
                descriptor.Rumble = [](void *userdata, Uint16 low, Uint16 high) {
                    auto &pad = *static_cast<NativeVirtualPad *>(userdata);
                    pad.low = low;
                    pad.high = high;
                    return true;
                };
            native = SDL_AttachVirtualJoystick(&descriptor);
            REQUIRE(native != 0);
            joystick = SDL_OpenJoystick(native);
            REQUIRE(joystick != nullptr);
        }

        ~NativeVirtualPad() {
            SDL_CloseJoystick(joystick);
            (void)SDL_DetachVirtualJoystick(native);
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }

        NativeVirtualPad(const NativeVirtualPad &) = delete;
        NativeVirtualPad &operator=(const NativeVirtualPad &) = delete;
    };

    TEST_CASE("SDL virtual gamepad routes native events and cancels rumble on focus loss", "[unit][input][sdl][gamepad]") {
        NativeVirtualPad native{true};
        SdlInputBackend backend;
        backend.BindWindow(7);
        backend.BeginFrame(1);
        backend.PollEvents();
        const auto &first = backend.Commit();
        const auto found = std::ranges::find(first.gamepads, native.name, &GamepadState::name);
        REQUIRE(found != first.gamepads.end());
        REQUIRE(found->canonicalMapping);
        const auto id = found->id;

        backend.BeginFrame(2);
        REQUIRE(SDL_SetJoystickVirtualButton(native.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
        REQUIRE(SDL_SetJoystickVirtualAxis(native.joystick, SDL_GAMEPAD_AXIS_LEFTX, 24575));
        SDL_UpdateJoysticks();
        backend.PollEvents();
        InputRouter router;
        const InputContextId contextId{"gameplay"};
        REQUIRE(router
                    .SetActionMap(
                        {{ActionId{"jump"}, ActionValueType::Digital, contextId, true, {{.kind = BindingControlKind::GamepadButton}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        router.BeginFrame(backend.Commit());
        REQUIRE(router.AssignGamepad(0, id));
        REQUIRE(router.ReadAction(context, ActionId{"jump"}, 0).pressed);
        REQUIRE(router.Snapshot().FindGamepad(id)->axes[0] > 0.7F);
        REQUIRE(backend.PlayRumble(id, {.lowFrequency = 0.5F, .highFrequency = 0.25F, .durationMilliseconds = 1000}).HasValue());
        REQUIRE(native.low != 0);
        REQUIRE(backend.PlayRumble(id, {.lowFrequency = std::numeric_limits<float>::quiet_NaN()}).HasError());

        backend.BeginFrame(3);
        SDL_Event focus{};
        focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        focus.window.windowID = 7;
        backend.ProcessEvent(focus);
        router.BeginFrame(backend.Commit());
        REQUIRE(native.low == 0);
        REQUIRE(native.high == 0);
        REQUIRE_FALSE(router.ReadAction(context, ActionId{"jump"}, 0).down);
        REQUIRE(backend.PlayRumble(id, {.lowFrequency = 0.5F, .durationMilliseconds = 1000}).HasError());
    }

    TEST_CASE("Unknown SDL joystick remains usable without optional mapping or glyph packages", "[unit][input][sdl][gamepad]") {
        NativeVirtualPad native{false, false};
        SdlInputBackend backend;
        backend.BeginFrame(1);
        backend.PollEvents();
        const auto &first = backend.Commit();
        const auto found = std::ranges::find(first.gamepads, native.name, &GamepadState::name);
        REQUIRE(found != first.gamepads.end());
        REQUIRE_FALSE(found->canonicalMapping);
        const auto id = found->id;
        backend.BeginFrame(2);
        REQUIRE(SDL_SetJoystickVirtualButton(native.joystick, 0, true));
        SDL_UpdateJoysticks();
        backend.PollEvents();
        InputRouter router;
        const InputContextId contextId{"menu"};
        REQUIRE(router
                    .SetActionMap({{ActionId{"confirm"},
                                    ActionValueType::Digital,
                                    contextId,
                                    true,
                                    {{.kind = BindingControlKind::RawGamepadButton, .rawControl = 0}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::FocusedGuiWidget);
        router.BeginFrame(backend.Commit());
        REQUIRE(router.AssignGamepad(1, id));
        REQUIRE(router.ReadAction(context, ActionId{"confirm"}, 1).pressed);
        REQUIRE(backend.PlayRumble(id, {.lowFrequency = 0.5F, .durationMilliseconds = 1000}).HasError());
        REQUIRE(CanonicalGlyph({BindingControlKind::RawGamepadButton, 0}).label == "Raw button 0");
    }

    TEST_CASE("Optional SDL mapping upgrades a raw device through a fresh canonical generation", "[unit][input][sdl][gamepad]") {
        NativeVirtualPad native{false, false};
        SdlInputBackend backend;
        backend.BeginFrame(1);
        backend.PollEvents();
        const auto &first = backend.Commit();
        const auto found = std::ranges::find(first.gamepads, native.name, &GamepadState::name);
        REQUIRE(found != first.gamepads.end());
        REQUIRE_FALSE(found->canonicalMapping);
        const auto old = found->id;
        REQUIRE(backend.InstallGamepadMapping("bad").HasError());
        std::array<char, 33> guid{};
        SDL_GUIDToString(SDL_GetJoystickGUID(native.joystick), guid.data(), static_cast<int>(guid.size()));
        const std::string mapping = std::string{guid.data()} + ",Horo mapped device,a:b0,leftx:a0,";
        REQUIRE(backend.InstallGamepadMapping(mapping).HasValue());
        backend.BeginFrame(2);
        backend.PollEvents();
        const auto &mapped = backend.Commit();
        REQUIRE(mapped.FindGamepad(old) == nullptr);
        const auto replacement = std::ranges::find_if(mapped.gamepads, [old](const GamepadState &pad) {
            return pad.id.slot == old.slot;
        });
        REQUIRE(replacement != mapped.gamepads.end());
        REQUIRE(replacement->canonicalMapping);
        REQUIRE(replacement->id != old);
        REQUIRE(backend.PlayRumble(old, {}).HasError());
    }
}  // namespace
