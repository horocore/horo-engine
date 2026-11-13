#include "Horo/Runtime/Input.h"
#include "support/AllocationProbe.h"
#include "support/OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thread>

namespace {
    using namespace Horo::Input;

    struct CaptureFixture {
        InputService input;
        InputContextId gameplayId{"gameplay"};
        InputContextId pauseId{"pause.rebind"};
        InputContextToken gameplay{input.Router().PushContext(gameplayId, InputContextKind::Gameplay)};
        InputContextToken owner;
        std::optional<InputBindingCapture> capture;
        GamepadDeviceId pad;
        GamepadDeviceId raw;
        FrameNumber frame{1};

        explicit CaptureFixture(const InputContextKind ownerKind = InputContextKind::ModalRoot)
            : owner(input.Router().PushContext(pauseId, ownerKind)) {
            REQUIRE(input.Router()
                        .SetActionMap({{ActionId{"jump"},
                                        ActionValueType::Digital,
                                        gameplayId,
                                        true,
                                        {{.kind = BindingControlKind::Key, .key = Key::Space}}},
                                       {ActionId{"pause.submit"},
                                        ActionValueType::Digital,
                                        pauseId,
                                        false,
                                        {{.kind = BindingControlKind::Key, .key = Key::J}}}})
                        .HasValue());
            input.BeginFrame(frame);
            pad = input.Collector().ConnectGamepad("canonical");
            raw = input.Collector().ConnectGamepad("unknown", false, 1, 1);
            REQUIRE(input.Collector().SetRawGamepadAxis(raw, 0, -1.0F));
            (void)input.CommitFrame();
            auto created = InputBindingCapture::Create(input.Router(), owner, ActionId{"jump"});
            REQUIRE(created.HasValue());
            capture.emplace(std::move(created).Value());
        }

        RawInputCollector &Begin() {
            input.BeginFrame(++frame);
            return input.Collector();
        }

        BindingCaptureResult Poll() {
            (void)input.CommitFrame();
            return capture->Poll(input.Router(), owner);
        }
    };

    TEST_CASE("Pause-menu and editor capture commit the same typed keyboard pointer and gamepad bindings", "[input][rebinding]") {
        CaptureFixture fixture;
        auto &collector = fixture.Begin();
        BindingControlKind expected{BindingControlKind::Key};
        SECTION("Keyboard") {
            collector.SetKey(Key::J, true);
            expected = BindingControlKind::Key;
        }
        SECTION("Pointer") {
            collector.SetPointerButton(PointerButton::Secondary, true);
            expected = BindingControlKind::PointerButton;
        }
        SECTION("Canonical gamepad") {
            REQUIRE(collector.SetGamepadButton(fixture.pad, GamepadButton::East, true));
            expected = BindingControlKind::GamepadButton;
        }
        SECTION("Unknown gamepad") {
            REQUIRE(collector.SetRawGamepadButton(fixture.raw, 0, true));
            expected = BindingControlKind::RawGamepadButton;
        }
        (void)fixture.input.CommitFrame();
        const auto before = Horo::Tests::AllocationProbe::Count();
        const auto result = fixture.capture->Poll(fixture.input.Router(), fixture.owner);
        const auto after = Horo::Tests::AllocationProbe::Count();
        REQUIRE(before == after);
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding);
        REQUIRE(result.binding->kind == expected);
        REQUIRE_FALSE(fixture.input.Router().ReadAction(fixture.owner, ActionId{"pause.submit"}).down);
        fixture.owner.Reset();
        REQUIRE_FALSE(fixture.input.Router().ReadAction(fixture.gameplay, ActionId{"jump"}).down);

        const Horo::Tests::OwnedTestDirectory directory{"horo-rebinding-session"};
        const BindingProfilePaths paths{{}, directory.Path() / "editor.json", directory.Path() / "project.json"};
        REQUIRE(CommitBindingProfileChange(fixture.input.Router(), paths, BindingProfileScope::ProjectUser,
                                           {ActionId{"jump"}, {*result.binding}})
                    .HasValue());
        const std::array ordered{paths.editorUser, paths.projectUser};
        const auto restored = LoadBindingProfileLayers(ordered);
        REQUIRE(restored.HasValue());
        REQUIRE(restored.Value().overrides.front().bindings.front() == *result.binding);
        REQUIRE_FALSE(std::filesystem::exists(paths.editorUser));
    }

    TEST_CASE("Resting unknown-controller axes require deliberate movement before capture", "[input][rebinding]") {
        CaptureFixture fixture;
        fixture.Begin();
        REQUIRE(fixture.Poll().status == BindingCaptureStatus::Pending);
        REQUIRE(fixture.capture->Poll(fixture.input.Router(), fixture.owner).status == BindingCaptureStatus::Pending);
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, 1.0F));
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding->kind == BindingControlKind::RawGamepadAxis);
        REQUIRE(result.binding->rawControl == 0);
    }

    TEST_CASE("Keyboard and pointer captures preserve modifiers without imposing them on gamepads", "[input][rebinding]") {
        CaptureFixture fixture;
        auto &collector = fixture.Begin();
        collector.SetModifiers({.control = true, .shift = true});
        bool gamepad{};
        SECTION("Key") {
            collector.SetKey(Key::K, true);
        }
        SECTION("Pointer button") {
            collector.SetPointerButton(PointerButton::Secondary, true);
        }
        SECTION("Horizontal wheel") {
            collector.AddPointerWheel(1.0F, 0.0F);
        }
        SECTION("Vertical wheel") {
            collector.AddPointerWheel(0.0F, 1.0F);
        }
        SECTION("Gamepad") {
            REQUIRE(collector.SetGamepadButton(fixture.pad, GamepadButton::East, true));
            gamepad = true;
        }
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding);
        const ModifierState expected = gamepad ? ModifierState{} : ModifierState{.control = true, .shift = true};
        CHECK(result.binding->requiredModifiers == expected);
    }

    TEST_CASE("Negative axis and wheel capture orientation follows the action value shape", "[input][rebinding]") {
        CaptureFixture fixture;
        const auto shape = GENERATE(ActionValueType::Digital, ActionValueType::Axis1D, ActionValueType::Axis2D);
        REQUIRE(
            fixture.input.Router()
                .SetActionMap({{ActionId{"jump"}, shape, fixture.gameplayId, true, {{.kind = BindingControlKind::Key, .key = Key::Space}}}})
                .HasValue());
        auto capture = InputBindingCapture::Create(fixture.input.Router(), fixture.owner, ActionId{"jump"});
        REQUIRE(capture.HasValue());
        fixture.capture.emplace(std::move(capture).Value());
        auto &collector = fixture.Begin();
        SECTION("Canonical axis") {
            REQUIRE(collector.SetGamepadAxis(fixture.pad, GamepadAxis::LeftX, -1.0F));
        }
        SECTION("Raw axis") {
            // The fixture's raw trigger starts at -1; move to center, then deliberately back.
            REQUIRE(collector.SetRawGamepadAxis(fixture.raw, 0, 0.0F));
            REQUIRE(fixture.Poll().status == BindingCaptureStatus::Pending);
            REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, -1.0F));
        }
        SECTION("Horizontal wheel") {
            collector.AddPointerWheel(-1.0F, 0.0F);
        }
        SECTION("Vertical wheel") {
            collector.AddPointerWheel(0.0F, -1.0F);
        }
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding);
        CHECK(result.binding->scale == (shape == ActionValueType::Digital ? 1.0F : -1.0F));
    }

    TEST_CASE("Rebinding cancels on Escape focus loss modal preemption and reused device generations", "[input][rebinding]") {
        CaptureFixture fixture;
        auto &collector = fixture.Begin();
        std::optional<InputContextToken> dialog;
        SECTION("Escape") {
            collector.SetKey(Key::Escape, true);
        }
        SECTION("Focus loss") {
            collector.SetWindowState({.focused = false});
        }
        SECTION("Modal preemption") {
            dialog.emplace(fixture.input.Router().PushContext(InputContextId{"dialog"}, InputContextKind::NativeDialog));
        }
        SECTION("Reused slot") {
            REQUIRE(collector.DisconnectGamepad(fixture.pad));
            const auto replacement = collector.ConnectGamepad("replacement");
            REQUIRE(replacement.slot == fixture.pad.slot);
            REQUIRE(replacement != fixture.pad);
        }
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Cancelled);
        REQUIRE_FALSE(result.binding);
        REQUIRE(fixture.capture->Poll(fixture.input.Router(), fixture.owner).status == BindingCaptureStatus::Cancelled);
    }

    TEST_CASE("A newer focused widget preempts binding capture without losing its own context", "[input][rebinding]") {
        CaptureFixture fixture{InputContextKind::FocusedGuiWidget};
        auto peer = fixture.input.Router().PushContext(InputContextId{"other.widget"}, InputContextKind::FocusedGuiWidget);
        fixture.Begin().SetKey(Key::J, true);
        REQUIRE(fixture.Poll().status == BindingCaptureStatus::Cancelled);
        fixture.owner.Reset();
        fixture.Begin();
        (void)fixture.input.CommitFrame();
        fixture.Begin();
        (void)fixture.input.CommitFrame();
        CHECK(fixture.input.Router().IsContextActive(peer));
        CHECK_FALSE(fixture.input.Router().IsContextActive(fixture.gameplay));
    }

    TEST_CASE("Moved and off-thread rebinding captures cannot mutate router ownership", "[input][rebinding]") {
        CaptureFixture fixture;
        auto moved = std::move(*fixture.capture);
        REQUIRE(fixture.capture->Poll(fixture.input.Router(), fixture.owner).status == BindingCaptureStatus::Cancelled);
        REQUIRE(fixture.input.Router().IsContextActive(fixture.owner));
        BindingCaptureResult result;
        std::thread foreign([&] {
            result = moved.Poll(fixture.input.Router(), fixture.owner);
        });
        foreign.join();
        REQUIRE(result.status == BindingCaptureStatus::Cancelled);
        REQUIRE(fixture.input.Router().IsContextActive(fixture.owner));
    }
}  // namespace
