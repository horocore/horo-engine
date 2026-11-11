#include "Horo/Runtime/Input.h"
#include "support/OwnedTestDirectory.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

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

    TEST_CASE("Touch contacts retain simultaneous edges and exact immutable incarnations", "[unit][runtime][input][touch]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        REQUIRE(collector.SetTouchContact({7, 1}, 10.0F, 20.0F, true) == TouchCollectionStatus::Accepted);
        REQUIRE(collector.SetTouchContact({8, 1}, 30.0F, 40.0F, true) == TouchCollectionStatus::Accepted);
        REQUIRE(collector.SetTouchContact({7, 1}, 11.0F, 21.0F, false) == TouchCollectionStatus::Accepted);
        const auto &first = collector.Commit();
        CHECK(first.touches[0].contact.pressed);
        CHECK(first.touches[0].contact.released);
        CHECK_FALSE(first.touches[0].contact.down);
        CHECK_FALSE(first.touches[0].cancelled);
        collector.BeginFrame(2);
        REQUIRE(collector.SetTouchContact({7, 2}, 12.0F, 22.0F, true) == TouchCollectionStatus::Accepted);
        CHECK((first.touches[0].id == TouchContactId{7, 1}));
        CHECK(first.touches[0].x == 11.0F);
        const auto &second = collector.Commit();
        CHECK((second.touches[0].id == TouchContactId{7, 2}));
        CHECK(second.touches[1].contact.down);
        CHECK_FALSE(second.touches[1].contact.pressed);
    }

    TEST_CASE("Touch overflow cancels all held contacts without silently dropping release evidence", "[unit][runtime][input][touch]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        for (std::size_t index = 0; index < MaximumTouchContacts; ++index)
            REQUIRE(collector.SetTouchContact({index + 1, 1}, 0.0F, 0.0F, true) == TouchCollectionStatus::Accepted);
        REQUIRE(collector.SetTouchContact({99, 1}, 0.0F, 0.0F, true) == TouchCollectionStatus::CapacityExceeded);
        REQUIRE(collector.SetTouchContact({1, 1}, 0.0F, 0.0F, false) == TouchCollectionStatus::CapacityExceeded);
        const auto &overflow = collector.Commit();
        CHECK(overflow.touchOverflow);
        for (const auto &touch : overflow.touches) {
            CHECK(touch.cancelled);
            CHECK(touch.contact.released);
            CHECK_FALSE(touch.contact.down);
        }
        collector.BeginFrame(2);
        REQUIRE(collector.SetTouchContact({99, 2}, 0.0F, 0.0F, true) == TouchCollectionStatus::Accepted);
        CHECK_FALSE(collector.Commit().touchOverflow);
    }

    TEST_CASE("Touch malformed, stale and focus-loss inputs cannot activate replacement contacts", "[unit][runtime][input][touch]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        CHECK(collector.SetTouchContact({}, 0.0F, 0.0F, true) == TouchCollectionStatus::Invalid);
        CHECK(collector.SetTouchContact({1, 1}, std::numeric_limits<float>::infinity(), 0.0F, true) == TouchCollectionStatus::Invalid);
        CHECK(collector.SetTouchContact({1, 1}, 0.0F, 0.0F, false) == TouchCollectionStatus::Stale);
        REQUIRE(collector.SetTouchContact({1, 1}, 0.0F, 0.0F, true) == TouchCollectionStatus::Accepted);
        REQUIRE(collector.SetTouchContact({1, 1}, 0.0F, 0.0F, false) == TouchCollectionStatus::Accepted);
        CHECK(collector.SetTouchContact({1, 1}, 0.0F, 0.0F, true) == TouchCollectionStatus::Stale);
        REQUIRE(collector.SetTouchContact({2, 1}, 0.0F, 0.0F, true) == TouchCollectionStatus::Accepted);
        collector.SetWindowState({.focused = false});
        CHECK(collector.SetTouchContact({3, 1}, 0.0F, 0.0F, true) == TouchCollectionStatus::Unavailable);
        const auto &lost = collector.Commit();
        CHECK(lost.touches[1].cancelled);
        CHECK(lost.touches[1].contact.released);
        CHECK_FALSE(lost.touches[1].contact.down);
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

    TEST_CASE("Explicit physical touch cancellation cannot become a release default", "[unit][runtime][input][touch]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        REQUIRE(collector.SetTouchContact({1, 1}, 10, 20, true) == TouchCollectionStatus::Accepted);
        REQUIRE(collector.SetTouchContact({2, 1}, 30, 40, true) == TouchCollectionStatus::Accepted);
        CHECK(collector.CancelTouchContact({1, 2}) == TouchCollectionStatus::Stale);
        REQUIRE(collector.CancelTouchContact({1, 1}) == TouchCollectionStatus::Accepted);
        const auto &first = collector.Commit();
        CHECK(first.touches[0].cancelled);
        CHECK(first.touches[0].contact.released);
        CHECK(first.touches[1].contact.down);
        collector.BeginFrame(2);
        collector.CancelTouchContacts(TouchCancellationReason::CapacityExceeded);
        CHECK(collector.SetTouchContact({3, 1}, 0, 0, true) == TouchCollectionStatus::CapacityExceeded);
        const auto &second = collector.Commit();
        CHECK(second.touchOverflow);
        CHECK(second.touches[1].cancelled);
        CHECK(first.touches[1].contact.down);
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
        builder.Capture(router, gameplay);
        const GameplayInputFrame first = builder.Consume(100);
        const GameplayInputFrame catchup = builder.Consume(101);
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
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-ü with spaces"};
        const std::filesystem::path path = directory.Path() / std::filesystem::path{u8"profil-é.json"};
        const InputBindingProfile profile{.profileId = "user", .overrides = {BindingOverride{ActionId{"jump"}, {KeyBinding(Key::Space)}}}};
        REQUIRE((SaveBindingProfileAtomically(path, profile).HasValue()));
        const Horo::Result<InputBindingProfile> loaded = LoadBindingProfile(path);
        REQUIRE((loaded.HasValue() && loaded.Value().profileId == "user"));
    }

    TEST_CASE("Profile numbers and chords cannot change identity during parsing", "[unit][runtime][input]") {
        const std::array invalidBindings{R"({"kind":256,"key":1})",
                                         R"({"kind":0.5,"key":1})",
                                         R"({"kind":0,"key":65537})",
                                         R"({"kind":6,"rawControl":65536})",
                                         R"({"kind":0,"key":1,"component":256})",
                                         R"({"kind":0,"key":1,"deadzoneKind":256})",
                                         R"({"kind":0,"key":1,"chord":[1,2,3,4,5]})",
                                         R"({"kind":0,"key":1,"chord":false})",
                                         R"({"kind":0,"key":1,"chord":[1],"chordSize":0})",
                                         R"({"kind":0,"key":-1})",
                                         R"({"kind":true,"key":1})"};
        for (const std::string_view binding : invalidBindings) {
            const std::string document =
                std::string{R"({"schemaVersion":1,"profileId":"user","overrides":[{"action":"jump","bindings":[)"} + std::string{binding} +
                "]}]}";
            INFO(document);
            REQUIRE(ParseBindingProfile(document).HasError());
        }
        REQUIRE(ParseBindingProfile(R"({"schemaVersion":4294967297,"profileId":"user","overrides":[]})").HasError());
        REQUIRE(ParseBindingProfile(R"({"schemaVersion":1.5,"profileId":"user","overrides":[]})").HasError());
    }

    TEST_CASE("Typed profile activation enforces the same storage bounds as persistence", "[unit][runtime][input]") {
        InputRouter router;
        const ActionId action{"action"};
        REQUIRE(
            router.SetActionMap({{action, ActionValueType::Digital, InputContextId{"workspace"}, false, {KeyBinding(Key::A)}}}).HasValue());
        const auto previous = router.Profile();
        InputBindingProfile invalid;
        invalid.profileId.clear();
        REQUIRE(router.SetProfile(invalid).HasError());
        REQUIRE(router.Profile() == previous);
        invalid.profileId.assign(257, 'a');
        REQUIRE(router.SetProfile(invalid).HasError());
        REQUIRE(router.Profile() == previous);
        invalid.profileId = "oversized";
        BindingOverride replacement{action, {}};
        for (std::size_t key = 1; key <= 65; ++key)
            replacement.bindings.push_back(KeyBinding(static_cast<Key>(key)));
        invalid.overrides.push_back(std::move(replacement));
        const std::array actions{
            ActionDescriptor{action, ActionValueType::Digital, InputContextId{"workspace"}, false, {KeyBinding(Key::A)}}};
        const auto validation = ValidateBindingProfile(actions, invalid);
        REQUIRE(std::ranges::any_of(validation.diagnostics, [](const BindingDiagnostic &diagnostic) {
            return diagnostic.code == BindingDiagnosticCode::MalformedProfile;
        }));
        REQUIRE(router.SetProfile(invalid).HasError());
        REQUIRE(router.Profile() == previous);
    }

    TEST_CASE("Invalid in-memory bindings cannot replace a persisted profile", "[unit][runtime][input]") {
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-recovery"};
        const auto path = directory.Path() / "input.json";
        InputBindingProfile profile{.profileId = "valid", .overrides = {{ActionId{"jump"}, {KeyBinding(Key::Space)}}}};
        REQUIRE(SaveBindingProfileAtomically(path, profile).HasValue());
        profile.profileId = "invalid";
        profile.overrides.front().bindings.front().chordSize = 255;
        REQUIRE(SaveBindingProfileAtomically(path, profile).HasError());
        const auto retained = LoadBindingProfile(path);
        REQUIRE(retained.HasValue());
        REQUIRE(retained.Value().profileId == "valid");
        REQUIRE(retained.Value().overrides.front().bindings.front().key == Key::Space);
    }

    TEST_CASE("Runtime and editor binding commits preserve scope and survive a new router session", "[unit][runtime][input]") {
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-layers"};
        const BindingProfilePaths paths{directory.Path() / "project/.horo/input.json", directory.Path() / "user/.horo/input/editor.json",
                                        directory.Path() / "user/.horo/input/projects/project-id.json"};
        const InputContextId contextId{"gameplay"};
        const ActionId jump{"jump"};
        const std::vector<ActionDescriptor> actions{{jump, ActionValueType::Digital, contextId, true, {KeyBinding(Key::Space)}}};
        REQUIRE(SaveBindingProfileAtomically(paths.projectDefaults, {.profileId = "project", .overrides = {{jump, {KeyBinding(Key::A)}}}})
                    .HasValue());
        InputRouter router;
        REQUIRE(router.SetActionMap(actions).HasValue());
        REQUIRE(CommitBindingProfileChange(router, paths, BindingProfileScope::EditorUser, {jump, {KeyBinding(Key::B)}}).HasValue());
        REQUIRE(router.Profile().overrides.front().bindings.front().key == Key::B);
        const InputBinding pointer{.kind = BindingControlKind::PointerButton};
        REQUIRE(CommitBindingProfileChange(router, paths, BindingProfileScope::ProjectUser, {jump, {pointer}}).HasValue());
        REQUIRE(LoadBindingProfile(paths.editorUser).Value().overrides.front().bindings.front().key == Key::B);
        REQUIRE(LoadBindingProfile(paths.projectDefaults).Value().overrides.front().bindings.front().key == Key::A);
        const std::array ordered{paths.projectDefaults, paths.editorUser, paths.projectUser};
        auto loaded = LoadBindingProfileLayers(ordered);
        REQUIRE(loaded.HasValue());
        InputRouter nextSession;
        REQUIRE(nextSession.SetActionMap(actions, std::move(loaded).Value()).HasValue());
        auto context = nextSession.PushContext(contextId, InputContextKind::Gameplay);
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetPointerButton(PointerButton::Primary, true);
        nextSession.BeginFrame(collector.Commit());
        REQUIRE(nextSession.ReadAction(context, jump).pressed);
        const InputBinding gamepad{.kind = BindingControlKind::GamepadButton};
        REQUIRE(CommitBindingProfileChange(nextSession, paths, BindingProfileScope::ProjectUser, {jump, {gamepad}}).HasValue());
        REQUIRE(LoadBindingProfile(paths.projectUser).Value().overrides.front().bindings.front().kind == BindingControlKind::GamepadButton);
    }

    TEST_CASE("Profile conflicts corruption and failed promotion retain the last active bindings", "[unit][runtime][input]") {
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-rollback"};
        const BindingProfilePaths paths{{}, directory.Path() / "editor.json", directory.Path() / "project.json"};
        const InputContextId contextId{"menu"};
        const ActionId first{"first"};
        InputRouter router;
        REQUIRE(router
                    .SetActionMap({{first, ActionValueType::Digital, contextId, true, {KeyBinding(Key::A)}},
                                   {ActionId{"second"}, ActionValueType::Digital, contextId, false, {KeyBinding(Key::B)}}})
                    .HasValue());
        REQUIRE(CommitBindingProfileChange(router, paths, BindingProfileScope::ProjectUser, {first, {KeyBinding(Key::C)}}).HasValue());
        REQUIRE(CommitBindingProfileChange(router, paths, BindingProfileScope::ProjectUser, {first, {KeyBinding(Key::B)}}).HasError());
        REQUIRE(LoadBindingProfile(paths.projectUser).Value().overrides.front().bindings.front().key == Key::C);
        REQUIRE(router.Profile().overrides.front().bindings.front().key == Key::C);
        std::ofstream(paths.editorUser) << "{broken";
        REQUIRE(CommitBindingProfileChange(router, paths, BindingProfileScope::ProjectUser, {first, {KeyBinding(Key::D)}}).HasError());
        REQUIRE(router.Profile().overrides.front().bindings.front().key == Key::C);
        std::filesystem::remove(paths.editorUser);
        const auto occupied = directory.Path() / "occupied";
        REQUIRE(std::filesystem::create_directory(occupied));
        REQUIRE(SaveBindingProfileAtomically(occupied, router.Profile()).HasError());
        REQUIRE(router.Profile().overrides.front().bindings.front().key == Key::C);
        REQUIRE(LoadBindingProfile(paths.projectUser).Value().overrides.front().bindings.front().key == Key::C);
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
        auto inheritedIdentity = higher;
        inheritedIdentity.profileId.clear();
        const auto inherited = MergeBindingProfiles(lower, inheritedIdentity);
        REQUIRE(inherited.HasValue());
        REQUIRE(inherited.Value().profileId == "project");
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

    TEST_CASE("Subframe gamepad taps retain both edges through production routing", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const InputContextId contextId{"gameplay"};
        const ActionId action{"jump"};
        REQUIRE(router.SetActionMap({{action, ActionValueType::Digital, contextId, false, {{.kind = BindingControlKind::GamepadButton}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        collector.BeginFrame(1);
        VirtualGamepad pad{collector};
        const auto id = pad.Connect();
        REQUIRE(pad.Press(GamepadButton::South));
        REQUIRE(pad.Release(GamepadButton::South));
        router.BeginFrame(collector.Commit());
        REQUIRE(router.AssignGamepad(0, id));
        const auto tap = router.ReadAction(context, action, 0);
        REQUIRE_FALSE(tap.down);
        REQUIRE(tap.pressed);
        REQUIRE(tap.released);
        REQUIRE_FALSE(router.ReadAction(context, action, 0).pressed);
        collector.BeginFrame(2);
        router.BeginFrame(collector.Commit());
        const auto next = router.ReadAction(context, action, 0);
        REQUIRE_FALSE(next.down);
        REQUIRE_FALSE(next.pressed);
        REQUIRE_FALSE(next.released);
    }

    TEST_CASE("Unfocused gamepad events cannot reactivate held controls on resume", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        const auto pad = collector.ConnectGamepad("canonical");
        const auto raw = collector.ConnectGamepad("unknown", false, 1, 1);
        REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::LeftX, 0.8F));
        collector.AddPointerWheel(0.0F, 1.0F);
        collector.SetWindowState({.focused = false});
        REQUIRE_FALSE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        REQUIRE_FALSE(collector.SetGamepadAxis(pad, GamepadAxis::LeftX, 0.9F));
        REQUIRE_FALSE(collector.SetRawGamepadButton(raw, 0, true));
        REQUIRE_FALSE(collector.SetRawGamepadAxis(raw, 0, 0.9F));
        (void)collector.Commit();
        collector.BeginFrame(2);
        collector.SetWindowState({.focused = true});
        const auto &snapshot = collector.Commit();
        REQUIRE_FALSE(snapshot.FindGamepad(pad)->buttons[0].down);
        REQUIRE(snapshot.FindGamepad(pad)->axes[0] == 0.0F);
        REQUIRE_FALSE(snapshot.FindGamepad(raw)->rawButtons[0].down);
        REQUIRE(snapshot.FindGamepad(raw)->rawAxes[0] == 0.0F);
        REQUIRE(snapshot.pointer.wheelY == 0.0F);
    }

    TEST_CASE("Gamepad trigger normalization preserves the canonical unsigned range", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        const auto pad = collector.ConnectGamepad("canonical");
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::LeftTrigger, -1.0F));
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::RightTrigger, 2.0F));
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::LeftX, -2.0F));
        const auto &snapshot = collector.Commit();
        const auto &axes = snapshot.FindGamepad(pad)->axes;
        REQUIRE(axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] == 0.0F);
        REQUIRE(axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] == 1.0F);
        REQUIRE(axes[static_cast<std::size_t>(GamepadAxis::LeftX)] == -1.0F);
    }

    TEST_CASE("Pointer device loss preserves keyboard and gamepad ownership", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetKey(Key::A, true);
        collector.SetPointerButton(PointerButton::Primary, true);
        const auto pad = collector.ConnectGamepad("held gamepad");
        REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        collector.SetWindowState({.pointerDeviceAvailable = false});
        collector.SetPointerButton(PointerButton::Primary, true);
        const auto &snapshot = collector.Commit();
        REQUIRE(snapshot.State(Key::A).down);
        REQUIRE(snapshot.FindGamepad(pad)->buttons[0].down);
        REQUIRE_FALSE(snapshot.State(PointerButton::Primary).down);
        REQUIRE_FALSE(snapshot.State(PointerButton::Primary).pressed);
        REQUIRE(snapshot.State(PointerButton::Primary).released);
    }

    TEST_CASE("Focus loss cannot turn a cancelled same-frame press into an activation on resume", "[unit][runtime][input]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        collector.SetKey(Key::A, true);
        const auto pad = collector.ConnectGamepad("pad");
        REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        collector.SetWindowState({.focused = false});
        collector.SetPointerPosition(100.0F, 200.0F);
        collector.SetWindowState({.focused = true});
        const auto &snapshot = collector.Commit();
        REQUIRE_FALSE(snapshot.State(Key::A).pressed);
        REQUIRE_FALSE(snapshot.State(Key::A).down);
        REQUIRE_FALSE(snapshot.FindGamepad(pad)->buttons[0].pressed);
        REQUIRE_FALSE(snapshot.FindGamepad(pad)->buttons[0].down);
        REQUIRE(snapshot.pointer.deltaX == 0.0F);
        REQUIRE(snapshot.pointer.deltaY == 0.0F);
    }

    TEST_CASE("Mixed keyboard and gamepad players own distinct controls and modifiers", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const ActionId action{"jump"};
        const InputContextId contextId{"gameplay"};
        REQUIRE(router
                    .SetActionMap({{action,
                                    ActionValueType::Digital,
                                    contextId,
                                    true,
                                    {KeyBinding(Key::A), {.kind = BindingControlKind::GamepadButton}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        collector.BeginFrame(1);
        collector.SetKey(Key::A, true);
        const auto pad = collector.ConnectGamepad("player two");
        router.BeginFrame(collector.Commit());
        REQUIRE(router.AssignGamepad(1, pad));
        REQUIRE(router.PlayerForKeyboardMouse() == 0);
        REQUIRE_FALSE(router.ReadAction(context, action, 1).down);
        REQUIRE(router.ReadAction(context, action, 0).pressed);
        collector.BeginFrame(2);
        collector.SetModifiers({.control = true});
        REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        router.BeginFrame(collector.Commit());
        REQUIRE(router.ReadAction(context, action, 1).pressed);
        REQUIRE_FALSE(router.ReadAction(context, action, 0).down);
        const auto before = router.RoutingState(context).assignmentRevision;
        router.AssignKeyboardMouse(2);
        REQUIRE(router.PlayerForKeyboardMouse() == 2);
        REQUIRE(router.RoutingState(context).assignmentRevision != before);
        collector.BeginFrame(3);
        collector.SetModifiers({});
        router.BeginFrame(collector.Commit());
        REQUIRE_FALSE(router.ReadAction(context, action, 0).down);
        REQUIRE(router.ReadAction(context, action, 2).down);
        router.AssignKeyboardMouse(std::nullopt);
        REQUIRE_FALSE(router.ReadAction(context, action, 2).down);
        REQUIRE(router.ReadAction(context, action).down);
    }

    TEST_CASE("Gamepad chords retain exact modifier policy and keyboard ownership", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const ActionId action{"chord"};
        const InputContextId contextId{"gameplay"};
        REQUIRE(router
                    .SetActionMap({{action,
                                    ActionValueType::Digital,
                                    contextId,
                                    false,
                                    {{.kind = BindingControlKind::GamepadButton, .chord = {Key::A}, .chordSize = 1}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        collector.BeginFrame(1);
        const auto pad = collector.ConnectGamepad("chord controller");
        collector.SetKey(Key::A, true);
        collector.SetModifiers({.control = true});
        REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        router.BeginFrame(collector.Commit());
        REQUIRE(router.AssignGamepad(1, pad));
        router.AssignKeyboardMouse(1);
        CHECK_FALSE(router.ReadAction(context, action, 1).down);
        collector.BeginFrame(2);
        collector.SetModifiers({});
        router.BeginFrame(collector.Commit());
        CHECK(router.ReadAction(context, action, 1).down);
        router.AssignKeyboardMouse(0);
        CHECK_FALSE(router.ReadAction(context, action, 1).down);
        CHECK(router.ReadAction(context, action).down);
    }

    TEST_CASE("Hot plug storms never transfer assignment to a reused gamepad slot", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const ActionId action{"jump"};
        const InputContextId contextId{"gameplay"};
        REQUIRE(router.SetActionMap({{action, ActionValueType::Digital, contextId, true, {{.kind = BindingControlKind::GamepadButton}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        GamepadDeviceId previous{};
        for (FrameNumber cycle = 0; cycle < 32; ++cycle) {
            collector.BeginFrame(cycle * 2 + 1);
            const auto pad = collector.ConnectGamepad("reconnected");
            REQUIRE(pad.IsValid());
            REQUIRE(pad != previous);
            REQUIRE_FALSE(collector.SetGamepadButton(previous, GamepadButton::South, true));
            REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
            router.BeginFrame(collector.Commit());
            REQUIRE_FALSE(router.ReadAction(context, action, 1).down);
            REQUIRE(router.AssignGamepad(1, pad));
            REQUIRE(router.ReadAction(context, action, 1).pressed);
            collector.BeginFrame(cycle * 2 + 2);
            REQUIRE(collector.DisconnectGamepad(pad));
            router.BeginFrame(collector.Commit());
            REQUIRE_FALSE(router.PlayerForGamepad(pad));
            REQUIRE_FALSE(router.ReadAction(context, action, 1).down);
            previous = pad;
        }
    }

    TEST_CASE("Replacing a collector cannot alias the previous backend's gamepad handle", "[unit][runtime][input]") {
        RawInputCollector oldBackend;
        oldBackend.BeginFrame(1);
        const auto old = oldBackend.ConnectGamepad("old backend");
        InputRouter router;
        router.BeginFrame(oldBackend.Commit());
        REQUIRE(router.AssignGamepad(1, old));
        RawInputCollector replacement;
        replacement.BeginFrame(2);
        const auto fresh = replacement.ConnectGamepad("replacement backend");
        REQUIRE(fresh.slot == old.slot);
        REQUIRE(fresh.sessionGeneration != old.sessionGeneration);
        REQUIRE_FALSE(replacement.SetGamepadButton(old, GamepadButton::South, true));
        router.BeginFrame(replacement.Commit());
        REQUIRE_FALSE(router.PlayerForGamepad(old));
        REQUIRE_FALSE(router.AssignGamepad(1, old));
        REQUIRE(router.AssignGamepad(1, fresh));
    }

    TEST_CASE("Disconnect releases the old player's action while preserving held keyboard input", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const ActionId action{"hold"};
        const InputContextId contextId{"gameplay"};
        REQUIRE(router
                    .SetActionMap({{action,
                                    ActionValueType::Digital,
                                    contextId,
                                    true,
                                    {KeyBinding(Key::A), {.kind = BindingControlKind::GamepadButton}}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        collector.BeginFrame(1);
        collector.SetKey(Key::A, true);
        const auto pad = collector.ConnectGamepad("player two");
        REQUIRE(collector.SetGamepadButton(pad, GamepadButton::South, true));
        router.BeginFrame(collector.Commit());
        REQUIRE(router.AssignGamepad(1, pad));
        REQUIRE(router.ReadAction(context, action, 1).down);
        collector.BeginFrame(2);
        REQUIRE(collector.DisconnectGamepad(pad));
        router.BeginFrame(collector.Commit());
        REQUIRE_FALSE(router.PlayerForGamepad(pad));
        const auto lost = router.ReadAction(context, action, 1);
        REQUIRE_FALSE(lost.down);
        REQUIRE(lost.released);
        const auto aggregate = router.ReadAction(context, action);
        REQUIRE(aggregate.down);
        REQUIRE(aggregate.released);
        collector.BeginFrame(3);
        router.BeginFrame(collector.Commit());
        REQUIRE_FALSE(router.ReadAction(context, action, 1).released);
    }

    TEST_CASE("Digital actions expose thresholded values and preserve subframe taps", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const InputContextId contextId{"gameplay"};
        const ActionId trigger{"trigger"};
        const ActionId tap{"tap"};
        REQUIRE(router
                    .SetActionMap(
                        {{trigger,
                          ActionValueType::Digital,
                          contextId,
                          false,
                          {{.kind = BindingControlKind::GamepadAxis, .gamepadAxis = GamepadAxis::LeftTrigger, .digitalThreshold = 0.5F}}},
                         {tap, ActionValueType::Digital, contextId, false, {KeyBinding(Key::A)}}})
                    .HasValue());
        auto context = router.PushContext(contextId, InputContextKind::Gameplay);
        collector.BeginFrame(1);
        const auto pad = collector.ConnectGamepad("trigger controller");
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::LeftTrigger, 0.25F));
        router.BeginFrame(collector.Commit());
        CHECK(router.ReadAction(context, trigger).x == 0.0F);
        collector.BeginFrame(2);
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::LeftTrigger, 0.75F));
        collector.SetKey(Key::A, true);
        collector.SetKey(Key::A, false);
        router.BeginFrame(collector.Commit());
        const auto thresholded = router.ReadAction(context, trigger);
        CHECK(thresholded.x == 1.0F);
        CHECK(thresholded.pressed);
        const auto pulse = router.ReadAction(context, tap);
        CHECK(pulse.x == 1.0F);
        CHECK(pulse.pressed);
        CHECK(pulse.released);
        CHECK_FALSE(pulse.down);
        collector.BeginFrame(3);
        REQUIRE(collector.SetGamepadAxis(pad, GamepadAxis::LeftTrigger, 0.0F));
        router.BeginFrame(collector.Commit());
        const auto released = router.ReadAction(context, trigger);
        CHECK(released.x == 0.0F);
        CHECK(released.released);
        CHECK(router.ReadAction(context, tap).x == 0.0F);
    }

    TEST_CASE("Project profile identity cannot escape the user root", "[unit][runtime][input]") {
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-containment"};
        for (const auto identity : {"../escape", "a/b", "a\\b", "C:escape", ".", "..", "CON", "nul.extra", "com1", "trailing.", "a:b"}) {
            INFO(identity);
            REQUIRE(ResolveProjectBindingProfilePath(directory.Path(), identity).HasError());
        }
        const auto resolved = ResolveProjectBindingProfilePath(directory.Path(), "stable-project_1");
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value() == directory.Path() / ".horo/input/projects/stable-project_1.json");
        REQUIRE(ResolveProjectBindingProfilePath({}, "project").HasError());
        const auto unicode = ResolveProjectBindingProfilePath(directory.Path(), "proje-ç");
        REQUIRE(unicode.HasValue());
        REQUIRE(unicode.Value().filename() == std::filesystem::path{u8"proje-ç.json"});
    }

    TEST_CASE("Profile layer resolution rejects symlinks escaping admitted roots", "[unit][runtime][input]") {
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-redirect"};
        const auto root = directory.Path() / "root";
        const auto outside = directory.Path() / "outside";
        std::filesystem::create_directories(root / ".horo");
        std::filesystem::create_directory(outside);
        std::error_code error;
        std::filesystem::create_directory_symlink(outside, root / ".horo/input", error);
        if (error)
            SKIP("Host does not permit directory symlink creation");
        REQUIRE(ResolveBindingProfilePaths({}, root).HasError());
        REQUIRE(ResolveProjectBindingProfilePath(root, "project").HasError());
        REQUIRE_FALSE(std::filesystem::exists(outside / "editor.json"));
    }

    TEST_CASE("Runtime profile reload retains inactive editor overrides on disk", "[unit][runtime][input]") {
        const Horo::Tests::OwnedTestDirectory directory{"horo-input-inactive"};
        const BindingProfilePaths paths{{}, directory.Path() / "editor.json", directory.Path() / "project.json"};
        REQUIRE(SaveBindingProfileAtomically(paths.editorUser,
                                             {.profileId = "global",
                                              .overrides = {{ActionId{"editor.save"}, {{.kind = BindingControlKind::Key, .key = Key::S}}}}})
                    .HasValue());
        InputRouter router;
        const InputContextId context{"gameplay"};
        REQUIRE(router.SetActionMap({{ActionId{"jump"}, ActionValueType::Digital, context, true, {KeyBinding(Key::Space)}}}).HasValue());
        REQUIRE(router.ReloadProfile(paths).HasValue());
        REQUIRE(router.Profile().overrides.empty());
        REQUIRE(CommitBindingProfileChange(router, paths, BindingProfileScope::ProjectUser, {ActionId{"jump"}, {KeyBinding(Key::J)}})
                    .HasValue());
        REQUIRE(LoadBindingProfile(paths.editorUser).Value().overrides.front().action == ActionId{"editor.save"});
        REQUIRE(router.Profile().overrides.size() == 1);
        REQUIRE(router.Profile().overrides.front().action == ActionId{"jump"});
    }
}  // namespace
