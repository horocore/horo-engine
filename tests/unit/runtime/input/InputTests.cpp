#include "Horo/Runtime/Input.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>

namespace {
    using namespace Horo::Input;

    struct CaptureOwner final : IInputCaptureOwner {
        void OnInputCaptureCancelled(const CaptureCancellationReason value) noexcept override {
            cancelled = value;
        }

        std::optional<CaptureCancellationReason> cancelled;
    };

    InputBinding KeyBinding(const Key key, const float scale = 1.0F, const std::uint8_t component = 0) {
        return InputBinding{.kind = BindingControlKind::Key, .key = key, .scale = scale, .component = component};
    }

    TEST_CASE("Snapshot Transitions Are Frame Local", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetKey(Key::W, true);
        const RawInputSnapshot &first = collector.Commit();
        REQUIRE((first.State(Key::W).down && first.State(Key::W).pressed && !first.State(Key::W).released));
        collector.BeginFrame(2);
        const RawInputSnapshot &held = collector.Commit();
        REQUIRE((held.State(Key::W).down && !held.State(Key::W).pressed && !held.State(Key::W).released));
        collector.BeginFrame(3);
        collector.SetKey(Key::W, false);
        const RawInputSnapshot &released = collector.Commit();
        REQUIRE((!released.State(Key::W).down && released.State(Key::W).released));
    }

    TEST_CASE("Input Service Commits Text And Ime Through The Same Snapshot", "[unit][runtime][input]") {
        InputService input;
        input.BeginFrame(7);
        input.Collector().AppendText("a");
        input.Collector().SetTextComposition("ö", 0, 1);
        const RawInputSnapshot &snapshot = input.CommitFrame();
        REQUIRE((snapshot.frame == 7 && snapshot.text == "a"));
        REQUIRE((snapshot.composition.active && snapshot.composition.text == "ö"));
        REQUIRE((&input.Router().Snapshot() == &snapshot));
    }

    TEST_CASE("Context Priority And Capture Are Deterministic", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetPointerButton(PointerButton::Primary, true);
        const RawInputSnapshot &snapshot = collector.Commit();
        InputRouter router;
        router.BeginFrame(snapshot);
        auto workspace = router.PushContext(InputContextId{"workspace"}, InputContextKind::EditorWorkspace);
        CaptureOwner owner;
        auto captured = router.CapturePointer(workspace, PointerButton::Primary, owner);
        REQUIRE((captured.HasValue() && router.HasCapture()));
        auto modal = router.PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        REQUIRE((owner.cancelled == CaptureCancellationReason::ModalOpened));
        REQUIRE((!router.IsContextActive(workspace) && router.IsContextActive(modal) && !router.HasCapture()));
        modal.Reset();
        REQUIRE_FALSE(router.IsContextActive(workspace));
        collector.BeginFrame(2);
        router.BeginFrame(collector.Commit());
        REQUIRE((router.IsContextActive(workspace)));
        auto firstWidget = router.PushContext(InputContextId{"widget.first"}, InputContextKind::FocusedGuiWidget);
        auto secondWidget = router.PushContext(InputContextId{"widget.second"}, InputContextKind::FocusedGuiWidget);
        REQUIRE((!router.IsContextActive(firstWidget) && router.IsContextActive(secondWidget)));
        secondWidget.Reset();
        REQUIRE((router.IsContextActive(firstWidget)));

        owner.cancelled.reset();
        auto escapeCapture = router.CapturePointer(firstWidget, PointerButton::Primary, owner);
        REQUIRE((escapeCapture.HasValue()));
        collector.BeginFrame(3);
        collector.SetKey(Key::Escape, true);
        router.BeginFrame(collector.Commit());
        REQUIRE((owner.cancelled == CaptureCancellationReason::Escape && !router.HasCapture()));

        owner.cancelled.reset();
        auto deviceCapture = router.CapturePointer(firstWidget, PointerButton::Primary, owner);
        REQUIRE((deviceCapture.HasValue()));
        collector.BeginFrame(4);
        collector.SetWindowState({.focused = true, .pointerInside = true, .pointerDeviceAvailable = false});
        router.BeginFrame(collector.Commit());
        REQUIRE((owner.cancelled == CaptureCancellationReason::DeviceDisconnected && !router.HasCapture()));

        owner.cancelled.reset();
        collector.BeginFrame(5);
        collector.SetWindowState({.focused = true, .pointerInside = true, .pointerDeviceAvailable = true});
        router.BeginFrame(collector.Commit());
        auto focusCapture = router.CapturePointer(firstWidget, PointerButton::Primary, owner);
        REQUIRE((focusCapture.HasValue()));
        collector.BeginFrame(6);
        collector.SetWindowState({.focused = false, .pointerInside = true, .pointerDeviceAvailable = true});
        router.BeginFrame(collector.Commit());
        REQUIRE((owner.cancelled == CaptureCancellationReason::FocusLost && !router.HasCapture()));

        collector.BeginFrame(7);
        collector.SetWindowState({.focused = true, .pointerInside = true, .pointerDeviceAvailable = true});
        router.BeginFrame(collector.Commit());
        for (const CaptureCancellationReason reason : {CaptureCancellationReason::Explicit, CaptureCancellationReason::OwnerDestroyed}) {
            owner.cancelled.reset();
            auto reasonCapture = router.CapturePointer(firstWidget, PointerButton::Primary, owner);
            REQUIRE((reasonCapture.HasValue()));
            router.CancelCapture(reason);
            REQUIRE((owner.cancelled == reason && !router.HasCapture()));
        }
        owner.cancelled.reset();
        auto releaseCapture = router.CapturePointer(firstWidget, PointerButton::Primary, owner);
        REQUIRE((releaseCapture.HasValue()));
        PointerCaptureToken releaseToken = std::move(releaseCapture).Value();
        releaseToken.Release();
        REQUIRE((!router.HasCapture() && !owner.cancelled.has_value()));

        auto context = router.PushContext(InputContextId{"temporary"}, InputContextKind::ModalChild);
        owner.cancelled.reset();
        auto contextCapture = router.CapturePointer(context, PointerButton::Primary, owner);
        REQUIRE((contextCapture.HasValue()));
        context.Reset();
        REQUIRE((owner.cancelled == CaptureCancellationReason::ContextRemoved && !router.HasCapture()));
    }

    TEST_CASE("Actions And Fixed Tick Edges Resolve Once", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(10);
        collector.SetKey(Key::W, true);
        collector.SetKey(Key::Space, true);
        const RawInputSnapshot &snapshot = collector.Commit();
        InputRouter router;
        const InputContextId gameplayId{"gameplay"};
        REQUIRE((router
                     .SetActionMap({
                         ActionDescriptor{ActionId{"move"}, ActionValueType::Axis2D, gameplayId, true, {KeyBinding(Key::W, 1.0F, 1)}},
                         ActionDescriptor{ActionId{"look"}, ActionValueType::Axis2D, gameplayId, false, {}},
                         ActionDescriptor{ActionId{"jump"}, ActionValueType::Digital, gameplayId, true, {KeyBinding(Key::Space)}},
                         ActionDescriptor{ActionId{"interact"}, ActionValueType::Digital, gameplayId, false, {}},
                     })
                     .HasValue()));
        router.BeginFrame(snapshot);
        auto gameplay = router.PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, ActionId{"look"}, ActionId{"jump"}, ActionId{"interact"}};
        const GameplayInputFrame first = builder.Consume(router, gameplay, 100);
        const GameplayInputFrame catchup = builder.Consume(router, gameplay, 101);
        REQUIRE((first.moveY == 1.0F && first.jumpPressed));
        REQUIRE((catchup.moveY == 1.0F && !catchup.jumpPressed));
    }

    TEST_CASE("Virtual Gamepad Uses Snapshot Path And Rejects Stale Ids", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        VirtualGamepad pad{collector};
        const GamepadDeviceId firstId = pad.Connect();
        REQUIRE((pad.Press(GamepadButton::South)));
        const RawInputSnapshot &connected = collector.Commit();
        REQUIRE((connected.FindGamepad(firstId) != nullptr));
        REQUIRE((connected.FindGamepad(firstId)->buttons[static_cast<std::size_t>(GamepadButton::South)].pressed));
        collector.BeginFrame(2);
        pad.Disconnect();
        REQUIRE((collector.Commit().FindGamepad(firstId) == nullptr));
        collector.BeginFrame(3);
        const GamepadDeviceId secondId = pad.Connect();
        REQUIRE((firstId.slot == secondId.slot && firstId.sessionGeneration != secondId.sessionGeneration));
        REQUIRE((!collector.SetGamepadButton(firstId, GamepadButton::South, true)));
    }

    TEST_CASE("Profiles Round Trip And Validate", "[unit][runtime][input]") {
        InputBindingProfile profile{.profileId = "desktop", .overrides = {BindingOverride{ActionId{"editor.save"}, {KeyBinding(Key::S)}}}};
        const auto serialized = SerializeBindingProfile(profile);
        REQUIRE((serialized.HasValue()));
        const auto parsed = ParseBindingProfile(serialized.Value());
        REQUIRE((parsed.HasValue() && parsed.Value().profileId == "desktop"));
        const std::array actions{
            ActionDescriptor{ActionId{"editor.save"}, ActionValueType::Digital, InputContextId{"workspace"}, true, {KeyBinding(Key::S)}}};
        REQUIRE((ValidateBindingProfile(actions, parsed.Value()).IsValid()));
        const Horo::Result<InputBindingProfile> malformed = ParseBindingProfile("{broken");
        REQUIRE((malformed.HasError()));
        REQUIRE((malformed.ErrorValue().domain.Value() == "horo.input"));
        REQUIRE((malformed.ErrorValue().code.Value() == "input.profile.malformed"));
        const Horo::Result<InputBindingProfile> duplicate =
            ParseBindingProfile(R"({"schemaVersion":1,"schemaVersion":1,"profileId":"bad","overrides":[]})");
        REQUIRE((duplicate.HasError()));
        REQUIRE((duplicate.ErrorValue().domain.Value() == "horo.input"));
        REQUIRE((duplicate.ErrorValue().code.Value() == "input.profile.malformed"));
    }

    TEST_CASE("Profiles Write Atomically To Non Ascii Paths", "[unit][runtime][input]") {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("horo-input-ü-" + std::to_string(suffix));
        const std::filesystem::path path = directory / "profil.json";
        const InputBindingProfile profile{.profileId = "user", .overrides = {BindingOverride{ActionId{"jump"}, {KeyBinding(Key::Space)}}}};
        REQUIRE((SaveBindingProfileAtomically(path, profile).HasValue()));
        const Horo::Result<InputBindingProfile> loaded = LoadBindingProfile(path);
        REQUIRE((loaded.HasValue() && loaded.Value().profileId == "user"));
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        REQUIRE((!error));
    }

    TEST_CASE("Profile Conflicts Reservations And Layering Are Typed", "[unit][runtime][input]") {
        const InputContextId workspace{"workspace"};
        const std::array actions{ActionDescriptor{ActionId{"first"}, ActionValueType::Digital, workspace, true, {KeyBinding(Key::A)}},
                                 ActionDescriptor{ActionId{"second"}, ActionValueType::Digital, workspace, false, {KeyBinding(Key::B)}}};
        InputBinding reserved = KeyBinding(Key::Q);
        reserved.requiredModifiers.command = true;
        const InputBindingProfile invalid{.profileId = "invalid",
                                          .overrides = {BindingOverride{ActionId{"first"}, {KeyBinding(Key::B)}},
                                                        BindingOverride{ActionId{"second"}, {reserved}}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, invalid);
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::any_of(report.diagnostics, [](const BindingDiagnostic &diagnostic) {
            return diagnostic.code == BindingDiagnosticCode::ReservedShortcut;
        })));

        const InputBindingProfile lower{.profileId = "project", .overrides = {BindingOverride{ActionId{"first"}, {KeyBinding(Key::C)}}}};
        const InputBindingProfile higher{.profileId = "user",
                                         .overrides = {BindingOverride{ActionId{"first"}, {KeyBinding(Key::D)}},
                                                       BindingOverride{ActionId{"second"}, {KeyBinding(Key::E)}}}};
        const auto merged = MergeBindingProfiles(lower, higher);
        REQUIRE((merged.HasValue() && merged.Value().profileId == "user" && merged.Value().overrides.size() == 2));
        REQUIRE((merged.Value().overrides.front().bindings.front().key == Key::D));
    }

    TEST_CASE("Conflict Validation Allows Same Trigger Across Contexts", "[unit][runtime][input]") {
        const InputContextId workspace{"workspace"};
        const InputContextId gameplay{"gameplay"};
        const std::array
            actions{ActionDescriptor{ActionId{"editor.interact"}, ActionValueType::Digital, workspace, false, {KeyBinding(Key::E)}},
                    ActionDescriptor{ActionId{"gameplay.use"}, ActionValueType::Digital, gameplay, false, {KeyBinding(Key::E)}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((report.IsValid()));
    }

    TEST_CASE("Conflict Validation Reports DuplicateBinding In The Same Context", "[unit][runtime][input]") {
        const InputContextId workspace{"workspace"};
        const std::array
            actions{ActionDescriptor{ActionId{"action.first"}, ActionValueType::Digital, workspace, false, {KeyBinding(Key::E)}},
                    ActionDescriptor{ActionId{"action.second"}, ActionValueType::Digital, workspace, false, {KeyBinding(Key::E)}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        const auto dupDiag = std::ranges::find(report.diagnostics, BindingDiagnosticCode::DuplicateBinding, &BindingDiagnostic::code);
        REQUIRE((dupDiag != report.diagnostics.end()));
        REQUIRE((dupDiag->message.find("action.second") != std::string::npos));
        REQUIRE((dupDiag->message.find("action.first") != std::string::npos));
        REQUIRE((dupDiag->message.find("workspace") != std::string::npos));
    }

    TEST_CASE("Conflict Validation Reports AmbiguousChord Overlap", "[unit][runtime][input]") {
        const InputContextId workspace{"workspace"};
        InputBinding baseChord = KeyBinding(Key::K);
        InputBinding subChord = KeyBinding(Key::K);
        subChord.chord[0] = Key::L;
        subChord.chordSize = 1;
        const std::array actions{ActionDescriptor{ActionId{"chord.base"}, ActionValueType::Digital, workspace, false, {baseChord}},
                                 ActionDescriptor{ActionId{"chord.sub"}, ActionValueType::Digital, workspace, false, {subChord}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        const auto chordDiag = std::ranges::find(report.diagnostics, BindingDiagnosticCode::AmbiguousChord, &BindingDiagnostic::code);
        REQUIRE((chordDiag != report.diagnostics.end()));
        REQUIRE((chordDiag->message.find("chord.sub") != std::string::npos));
        REQUIRE((chordDiag->message.find("chord.base") != std::string::npos));
    }

    TEST_CASE("Conflict Validation Reports Gamepad Axis Exclusivity", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        InputBinding axisA{.kind = BindingControlKind::GamepadAxis, .gamepadAxis = GamepadAxis::LeftX, .scale = 1.0F};
        InputBinding axisB{.kind = BindingControlKind::GamepadAxis, .gamepadAxis = GamepadAxis::LeftX, .scale = -1.0F};
        const std::array actions{ActionDescriptor{ActionId{"steer.left"}, ActionValueType::Axis1D, gameplay, false, {axisA}},
                                 ActionDescriptor{ActionId{"steer.right"}, ActionValueType::Axis1D, gameplay, false, {axisB}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        const auto axisDiag =
            std::ranges::find(report.diagnostics, BindingDiagnosticCode::DeviceExclusivityViolation, &BindingDiagnostic::code);
        REQUIRE((axisDiag != report.diagnostics.end()));
        REQUIRE((axisDiag->message.find("gameplay") != std::string::npos));
    }

    TEST_CASE("Conflict Validation Reports Raw Axis Exclusivity", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        InputBinding rawA{.kind = BindingControlKind::RawGamepadAxis, .rawControl = 3, .scale = 1.0F};
        InputBinding rawB{.kind = BindingControlKind::RawGamepadAxis, .rawControl = 3, .scale = -1.0F};
        const std::array actions{ActionDescriptor{ActionId{"raw.left"}, ActionValueType::Axis1D, gameplay, false, {rawA}},
                                 ActionDescriptor{ActionId{"raw.right"}, ActionValueType::Axis1D, gameplay, false, {rawB}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::any_of(report.diagnostics, [](const BindingDiagnostic &d) {
            return d.code == BindingDiagnosticCode::DeviceExclusivityViolation;
        })));
    }

    TEST_CASE("Conflict Validation Reports Pointer Wheel Exclusivity", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        InputBinding wheelA{.kind = BindingControlKind::PointerWheelX, .scale = 1.0F};
        InputBinding wheelB{.kind = BindingControlKind::PointerWheelX, .scale = -1.0F};
        const std::array xActions{ActionDescriptor{ActionId{"look.x"}, ActionValueType::Axis1D, gameplay, false, {wheelA}},
                                  ActionDescriptor{ActionId{"zoom.x"}, ActionValueType::Axis1D, gameplay, false, {wheelB}}};
        REQUIRE((!ValidateBindingProfile(xActions, InputBindingProfile{}).IsValid()));
        InputBinding wheelYA{.kind = BindingControlKind::PointerWheelY, .scale = 1.0F};
        InputBinding wheelYB{.kind = BindingControlKind::PointerWheelY, .scale = -1.0F};
        const std::array yActions{ActionDescriptor{ActionId{"look.y"}, ActionValueType::Axis1D, gameplay, false, {wheelYA}},
                                  ActionDescriptor{ActionId{"zoom.y"}, ActionValueType::Axis1D, gameplay, false, {wheelYB}}};
        REQUIRE((!ValidateBindingProfile(yActions, InputBindingProfile{}).IsValid()));
    }

    TEST_CASE("Conflict Validation Rejects Invalid Component On Digital And Axis1D", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        InputBinding invalidComponent = KeyBinding(Key::Space);
        invalidComponent.component = 1;
        const std::array digital{ActionDescriptor{ActionId{"jump"}, ActionValueType::Digital, gameplay, false, {invalidComponent}}};
        REQUIRE((!ValidateBindingProfile(digital, InputBindingProfile{}).IsValid()));
        InputBinding invalidAxis{.kind = BindingControlKind::GamepadAxis, .gamepadAxis = GamepadAxis::LeftX, .component = 1};
        const std::array axis{ActionDescriptor{ActionId{"move.x"}, ActionValueType::Axis1D, gameplay, false, {invalidAxis}}};
        REQUIRE((!ValidateBindingProfile(axis, InputBindingProfile{}).IsValid()));
    }

    TEST_CASE("Conflict Validation Skips Bindings When Context Is Invalid", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        InputBinding invalidDeadzone = KeyBinding(Key::Space);
        invalidDeadzone.deadzone = 2.0F;
        const std::array
            actions{ActionDescriptor{ActionId{"valid.action"}, ActionValueType::Digital, InputContextId{""}, false, {invalidDeadzone}},
                    ActionDescriptor{ActionId{"other.action"}, ActionValueType::Digital, gameplay, false, {KeyBinding(Key::Space)}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::count_if(report.diagnostics, [](const BindingDiagnostic &d) {
            return d.code == BindingDiagnosticCode::AmbiguousContext;
        }) == 1));
        REQUIRE((std::ranges::none_of(report.diagnostics, [](const BindingDiagnostic &d) {
            return d.action.Value() == "valid.action" && d.code != BindingDiagnosticCode::AmbiguousContext;
        })));
    }

    TEST_CASE("Conflict Validation Reports Empty Action Id", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        const std::array actions{ActionDescriptor{ActionId{""}, ActionValueType::Digital, gameplay, false, {KeyBinding(Key::Space)}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::any_of(report.diagnostics, [](const BindingDiagnostic &d) {
            return d.code == BindingDiagnosticCode::InvalidAction;
        })));
    }

    TEST_CASE("Conflict Validation Rejects Duplicate Action Ids", "[unit][runtime][input]") {
        const InputContextId workspace{"workspace"};
        const InputContextId gameplay{"gameplay"};
        const std::array
            actions{ActionDescriptor{ActionId{"shared.action"}, ActionValueType::Digital, workspace, false, {KeyBinding(Key::A)}},
                    ActionDescriptor{ActionId{"shared.action"}, ActionValueType::Digital, gameplay, false, {KeyBinding(Key::B)}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::any_of(report.diagnostics, [](const BindingDiagnostic &diagnostic) {
            return diagnostic.code == BindingDiagnosticCode::InvalidAction && diagnostic.message.find("shared.action") != std::string::npos;
        })));
    }

    TEST_CASE("Conflict Validation Rejects Oversized Chords Without Inspecting Past Storage", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        InputBinding malformed = KeyBinding(Key::K);
        malformed.chordSize = static_cast<std::uint8_t>(malformed.chord.size() + 1);
        const std::array
            actions{ActionDescriptor{ActionId{"malformed.chord"}, ActionValueType::Digital, gameplay, false, {malformed}},
                    ActionDescriptor{ActionId{"valid.chord"}, ActionValueType::Digital, gameplay, false, {KeyBinding(Key::K)}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::any_of(report.diagnostics, [](const BindingDiagnostic &diagnostic) {
            return diagnostic.action.Value() == "malformed.chord" && diagnostic.code == BindingDiagnosticCode::AmbiguousChord;
        })));
    }

    TEST_CASE("Conflict Validation Reports Duplicate Overrides For One Action", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        const std::array actions{ActionDescriptor{ActionId{"jump"}, ActionValueType::Digital, gameplay, false, {KeyBinding(Key::Space)}}};
        const InputBindingProfile profile{.profileId = "dup-override",
                                          .overrides = {BindingOverride{ActionId{"jump"}, {KeyBinding(Key::A)}},
                                                        BindingOverride{ActionId{"jump"}, {KeyBinding(Key::B)}}}};
        const BindingValidationReport report = ValidateBindingProfile(actions, profile);
        REQUIRE((!report.IsValid()));
        REQUIRE((std::ranges::any_of(report.diagnostics, [](const BindingDiagnostic &d) {
            return d.code == BindingDiagnosticCode::DuplicateBinding && d.message.find("jump") != std::string::npos;
        })));
    }

    TEST_CASE("Conflict Validation Reports Duplicate Triggers Inside One Action", "[unit][runtime][input]") {
        const InputContextId gameplay{"gameplay"};
        const std::array actions{ActionDescriptor{ActionId{"jump"},
                                                  ActionValueType::Digital,
                                                  gameplay,
                                                  false,
                                                  {KeyBinding(Key::Space), KeyBinding(Key::Space)}}};
        const BindingValidationReport defaultReport = ValidateBindingProfile(actions, InputBindingProfile{});
        REQUIRE((!defaultReport.IsValid()));
        REQUIRE((std::ranges::any_of(defaultReport.diagnostics, [](const BindingDiagnostic &d) {
            return d.code == BindingDiagnosticCode::DuplicateBinding && d.message.find("jump") != std::string::npos;
        })));
        const InputBindingProfile overrideProfile{.profileId = "dup-binding",
                                                  .overrides = {
                                                      BindingOverride{ActionId{"jump"}, {KeyBinding(Key::A), KeyBinding(Key::A)}}}};
        const BindingValidationReport overrideReport = ValidateBindingProfile(actions, overrideProfile);
        REQUIRE((!overrideReport.IsValid()));
        REQUIRE((std::ranges::any_of(overrideReport.diagnostics, [](const BindingDiagnostic &d) {
            return d.code == BindingDiagnosticCode::DuplicateBinding && d.message.find("override") != std::string::npos;
        })));
    }

    TEST_CASE("InputRouter Rejects Invalid Profile Without Mutating Live State", "[unit][runtime][input]") {
        const InputContextId workspace{"workspace"};
        InputRouter router;
        REQUIRE(
            (router.SetActionMap({ActionDescriptor{ActionId{"test.act"}, ActionValueType::Digital, workspace, false, {KeyBinding(Key::Z)}}})
                 .HasValue()));
        InputBindingProfile badProfile{.profileId = "bad", .overrides = {BindingOverride{ActionId{"unknown.act"}, {KeyBinding(Key::X)}}}};
        const Horo::Result<void> setRes = router.SetProfile(badProfile);
        REQUIRE((setRes.HasError()));
        REQUIRE((router.Profile().profileId == "default"));
    }

    TEST_CASE("Action Transitions Are Consumed And Gamepad Axes Have Edges", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetKey(Key::A, true);
        const GamepadDeviceId gamepad = collector.ConnectGamepad("pad");
        REQUIRE((collector.SetGamepadAxis(gamepad, GamepadAxis::LeftX, 0.8F)));
        InputRouter router;
        const InputContextId contextId{"gameplay"};
        InputBinding axis{.kind = BindingControlKind::GamepadAxis,
                          .gamepadAxis = GamepadAxis::LeftX,
                          .deadzoneKind = DeadzoneKind::Axial,
                          .deadzone = 0.1F};
        REQUIRE((router
                     .SetActionMap({ActionDescriptor{ActionId{"key"}, ActionValueType::Digital, contextId, false, {KeyBinding(Key::A)}},
                                    ActionDescriptor{ActionId{"axis"}, ActionValueType::Axis1D, contextId, false, {axis}}})
                     .HasValue()));
        router.BeginFrame(collector.Commit());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        REQUIRE((router.ReadAction(context, ActionId{"key"}).pressed));
        REQUIRE((!router.ReadAction(context, ActionId{"key"}).pressed));
        REQUIRE((router.ReadAction(context, ActionId{"axis"}).pressed));

        collector.BeginFrame(2);
        router.BeginFrame(collector.Commit());
        REQUIRE((router.ReadAction(context, ActionId{"axis"}).down));
        REQUIRE((!router.ReadAction(context, ActionId{"axis"}).pressed));
        collector.BeginFrame(3);
        REQUIRE((collector.SetGamepadAxis(gamepad, GamepadAxis::LeftX, 0.0F)));
        router.BeginFrame(collector.Commit());
        REQUIRE((router.ReadAction(context, ActionId{"axis"}).released));
    }

    TEST_CASE("Neutralize Releases Held Controls And Prunes Assignments", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetKey(Key::W, true);
        const GamepadDeviceId gamepad = collector.ConnectGamepad("pad");
        InputRouter router;
        router.BeginFrame(collector.Commit());
        REQUIRE((router.AssignGamepad(0, gamepad)));
        collector.BeginFrame(2);
        collector.Neutralize();
        router.BeginFrame(collector.Commit());
        REQUIRE((router.Snapshot().State(Key::W).released));
        collector.BeginFrame(3);
        REQUIRE((collector.DisconnectGamepad(gamepad)));
        router.BeginFrame(collector.Commit());
        REQUIRE((!router.PlayerForGamepad(gamepad).has_value()));
    }

    TEST_CASE("Recording Replays Exactly", "[unit][runtime][input]") {
        GameplayInputRecording recording;
        recording.Record(GameplayInputFrame{.tick = 1, .moveX = 0.5F, .jumpPressed = true});
        recording.Record(GameplayInputFrame{.tick = 2, .lookY = -0.25F});
        REQUIRE((recording.Next() == recording.Frames()[0]));
        REQUIRE((recording.Next() == recording.Frames()[1]));
        REQUIRE((!recording.Next().has_value()));
        recording.ResetReplay();
        REQUIRE((recording.Next() == recording.Frames()[0]));
    }
}  // namespace
