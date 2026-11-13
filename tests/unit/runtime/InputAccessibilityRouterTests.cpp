#include "Horo/Runtime/InputAccessibility.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Input {
    namespace {
        struct MappingFixture final {
            InputService input;
            InputContextToken context;
            ActionId action{"play.accessible"};
            std::uint64_t frame{};

            explicit MappingFixture(const BindingAccessibilityPolicy policy) {
                InputBinding binding{.kind = BindingControlKind::Key, .key = Key::K};
                binding.accessibility = policy;
                REQUIRE(
                    input.Router().SetActionMap({{action, ActionValueType::Digital, InputContextId{"play"}, true, {binding}}}).HasValue());
                context = input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
            }

            void Begin(const std::optional<InputSampleTime> time) {
                input.Router().EndFrame();
                input.BeginFrame(++frame, time);
            }

            ActionEvidence Read() {
                return input.Router().ReadActionEvidence(context, action);
            }

            ActionEvidence Sample(const std::optional<InputSampleTime> time, const bool down) {
                Begin(time);
                input.Collector().SetKey(Key::K, down);
                static_cast<void>(input.CommitFrame());
                return Read();
            }
        };
    }  // namespace

    TEST_CASE("Router toggle owns semantic state without changing raw facts or replaying an edge on repeated reads",
              "[input][accessibility][router][allocation]") {
        MappingFixture fixture{{.digital = {.toggle = true}}};
        REQUIRE_FALSE(fixture.Sample(std::nullopt, false).value.down);
        fixture.Begin(std::nullopt);
        fixture.input.Collector().SetKey(Key::K, true);
        const auto &raw = fixture.input.CommitFrame();
        Horo::Tests::AllocationProbe::Measurement allocation;
        ActionEvidence pressed, repeatedRead;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            pressed = fixture.Read();
            repeatedRead = fixture.Read();
            allocation = probe.Snapshot();
        }
        REQUIRE(allocation.requests == 0);
        REQUIRE(pressed.value.down);
        REQUIRE(pressed.value.pressed);
        REQUIRE(repeatedRead.value.down);
        REQUIRE_FALSE(repeatedRead.value.pressed);
        REQUIRE(raw.State(Key::K).pressed);
        REQUIRE(fixture.Sample(std::nullopt, false).value.down);
        const auto off = fixture.Sample(std::nullopt, true);
        REQUIRE_FALSE(off.value.down);
        REQUIRE(off.value.released);
    }

    TEST_CASE("Accessibility setup admits exact mapping bounds and rejects larger identities or binding inventories",
              "[input][accessibility][router][validation]") {
        InputService input;
        ActionDescriptor action{ActionId{std::string(MaximumAccessibilityActionIdBytes, 'a')},
                                ActionValueType::Digital,
                                InputContextId{"play"},
                                false,
                                {}};
        for (std::size_t index = 0; index < MaximumAccessibilityBindingsPerAction; ++index) {
            InputBinding binding{.kind = BindingControlKind::RawGamepadButton, .rawControl = static_cast<std::uint16_t>(index)};
            binding.accessibility.digital.toggle = true;
            action.defaultBindings.push_back(binding);
        }
        REQUIRE(input.Router().SetActionMap({action}).HasValue());
        action.id = ActionId{std::string(MaximumAccessibilityActionIdBytes + 1, 'a')};
        REQUIRE(input.Router().SetActionMap({action}).HasError());
        REQUIRE(input.Router().Actions()[0].id.Value().size() == MaximumAccessibilityActionIdBytes);
        action.id = ActionId{"play.capacity"};
        auto extra = action.defaultBindings.back();
        ++extra.rawControl;
        action.defaultBindings.push_back(extra);
        REQUIRE(input.Router().SetActionMap({action}).HasError());
        REQUIRE(input.Router().Actions()[0].defaultBindings.size() == MaximumAccessibilityBindingsPerAction);
    }

    TEST_CASE("Router hold and repeat use supplied time with distinct activation and repeat edges",
              "[input][accessibility][router][time]") {
        MappingFixture fixture{
            {.digital = {.holdMicroseconds = 100, .repeat = true, .repeatDelayMicroseconds = 50, .repeatIntervalMicroseconds = 20}}};
        REQUIRE_FALSE(fixture.Sample(InputSampleTime{0}, false).value.down);
        REQUIRE_FALSE(fixture.Sample(InputSampleTime{10}, true).value.down);
        REQUIRE_FALSE(fixture.Sample(InputSampleTime{109}, true).value.down);
        const auto held = fixture.Sample(InputSampleTime{110}, true);
        REQUIRE(held.value.pressed);
        REQUIRE(held.value.down);
        REQUIRE_FALSE(held.value.repeated);
        const auto repeat = fixture.Sample(InputSampleTime{160}, true);
        REQUIRE(repeat.value.repeated);
        REQUIRE_FALSE(repeat.value.pressed);
        REQUIRE_FALSE(fixture.Read().value.repeated);
        REQUIRE_FALSE(fixture.Sample(InputSampleTime{170}, true).value.repeated);
        REQUIRE(fixture.Sample(InputSampleTime{180}, true).value.repeated);
        REQUIRE(fixture.Sample(InputSampleTime{190}, false).value.released);
        REQUIRE(fixture.Sample(std::nullopt, true).status == ActionReadStatus::ClockUnavailable);
    }

    TEST_CASE("Timed cancellation releases retain missing clock cause independently of the current action map",
              "[input][accessibility][router][time][lifetime]") {
        MappingFixture fixture{{.digital = {.holdMicroseconds = 10}}};
        static_cast<void>(fixture.Sample(InputSampleTime{0}, false));
        static_cast<void>(fixture.Sample(InputSampleTime{1}, true));
        REQUIRE(fixture.Sample(InputSampleTime{11}, true).value.down);
        SECTION("The action is still held when host time disappears") {}
        SECTION("An unread physical release precedes the missing clock") {
            fixture.Begin(InputSampleTime{12});
            fixture.input.Collector().SetKey(Key::K, false);
            static_cast<void>(fixture.input.CommitFrame());
        }
        fixture.Begin(std::nullopt);
        static_cast<void>(fixture.input.CommitFrame());
        bool removed = false;
        SECTION("The existing binding still requires host time") {}
        SECTION("The old action map is removed before its release is observed") {
            REQUIRE(fixture.input.Router().SetActionMap({}).HasValue());
            removed = true;
        }
        const auto release = fixture.Read();
        REQUIRE(release.status == ActionReadStatus::ClockUnavailable);
        REQUIRE(release.value.released);
        REQUIRE_FALSE(release.value.down);
        const auto again = fixture.Read();
        REQUIRE_FALSE(again.value.released);
        REQUIRE(again.status == (removed ? ActionReadStatus::Unavailable : ActionReadStatus::ClockUnavailable));
    }

    TEST_CASE("Router observes releases even when an action skips reads and requires the next hold anew",
              "[input][accessibility][router][lifecycle]") {
        MappingFixture fixture{{.digital = {.holdMicroseconds = 10}}};
        static_cast<void>(fixture.Sample(InputSampleTime{0}, false));
        static_cast<void>(fixture.Sample(InputSampleTime{1}, true));
        REQUIRE(fixture.Sample(InputSampleTime{11}, true).value.down);
        fixture.Begin(InputSampleTime{12});
        fixture.input.Collector().SetKey(Key::K, false);
        static_cast<void>(fixture.input.CommitFrame());
        fixture.Begin(InputSampleTime{13});
        fixture.input.Collector().SetKey(Key::K, true);
        static_cast<void>(fixture.input.CommitFrame());
        REQUIRE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.down);
        REQUIRE_FALSE(fixture.Sample(InputSampleTime{22}, true).value.down);
        REQUIRE(fixture.Sample(InputSampleTime{23}, true).value.pressed);
    }

    TEST_CASE("Modal preemption focus loss replacement and backwards time withdraw the old synthetic incarnation",
              "[input][accessibility][router][lifecycle]") {
        MappingFixture fixture{{.digital = {.toggle = true}}};
        static_cast<void>(fixture.Sample(InputSampleTime{0}, false));
        REQUIRE(fixture.Sample(InputSampleTime{1}, true).value.down);
        static_cast<void>(fixture.Sample(InputSampleTime{2}, false));
        auto modal = fixture.input.Router().PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        REQUIRE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.down);
        modal.Reset();
        static_cast<void>(fixture.Sample(InputSampleTime{3}, false));
        REQUIRE(fixture.Sample(InputSampleTime{4}, true).value.down);
        REQUIRE(fixture.Sample(InputSampleTime{1}, true).value.released);
        REQUIRE_FALSE(fixture.Sample(InputSampleTime{2}, true).value.down);
        static_cast<void>(fixture.Sample(InputSampleTime{3}, false));
        REQUIRE(fixture.Sample(InputSampleTime{4}, true).value.down);
        fixture.Begin(InputSampleTime{5});
        fixture.input.Collector().SetWindowState({.focused = false});
        static_cast<void>(fixture.input.CommitFrame());
        REQUIRE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.down);
    }

    TEST_CASE("Retired action-name leases survive action-map replacement until their original context drains release",
              "[input][accessibility][router][lifetime]") {
        MappingFixture fixture{{.digital = {.toggle = true}}};
        static_cast<void>(fixture.Sample(std::nullopt, false));
        REQUIRE(fixture.Sample(std::nullopt, true).value.down);
        REQUIRE(fixture.input.Router().SetActionMap({}).HasValue());
        REQUIRE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.down);
        fixture.context.Reset();
        auto next = fixture.input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
        REQUIRE_FALSE(fixture.input.Router().ReadAction(next, fixture.action).released);
    }

    TEST_CASE("Ended contexts emit scoped owned withdrawals without invoking dead owners or leaking into replacement contexts",
              "[input][accessibility][router][withdrawal]") {
        RetiredInputActionRelease retained;
        {
            MappingFixture fixture{{.digital = {.toggle = true}}};
            InputBinding first{.kind = BindingControlKind::Key, .key = Key::K};
            first.accessibility.digital.toggle = true;
            auto second = first;
            second.key = Key::L;
            REQUIRE(fixture.input.Router().SetProfile({.overrides = {{fixture.action, {first, second}}}}).HasValue());
            static_cast<void>(fixture.Sample(std::nullopt, false));
            fixture.Begin(std::nullopt);
            fixture.input.Collector().SetKey(Key::K, true);
            fixture.input.Collector().SetKey(Key::L, true);
            static_cast<void>(fixture.input.CommitFrame());
            REQUIRE(fixture.Read().value.down);
            const auto oldIdentity = fixture.input.Router().RoutingState(fixture.context).contextIdentity;
            fixture.context.Reset();
            auto replacement = fixture.input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
            REQUIRE(fixture.input.Router().RoutingState(replacement).contextIdentity != oldIdentity);
            REQUIRE_FALSE(fixture.input.Router().ReadAction(replacement, fixture.action).down);
            REQUIRE(fixture.input.Router().TryDequeueRetiredActionRelease(retained));
            REQUIRE(retained.contextIdentity == oldIdentity);
            REQUIRE(retained.action);
            REQUIRE(*retained.action == fixture.action);
            REQUIRE_FALSE(retained.player.has_value());
            auto unchanged = retained;
            REQUIRE_FALSE(fixture.input.Router().TryDequeueRetiredActionRelease(unchanged));
            REQUIRE(unchanged.contextIdentity == retained.contextIdentity);
            REQUIRE(unchanged.action == retained.action);
            fixture.input.Router().EndFrame();
        }
        REQUIRE(retained.action->Value() == "play.accessible");
    }

    TEST_CASE("Router sticky modifiers admit sequential chords and keep an admitted hold until physical release",
              "[input][accessibility][router][sticky]") {
        MappingFixture fixture{{.stickyModifiers = true}};
        InputBinding binding{.kind = BindingControlKind::Key, .key = Key::K, .requiredModifiers = {.control = true}};
        binding.accessibility.stickyModifiers = true;
        REQUIRE(fixture.input.Router().SetProfile({.overrides = {{fixture.action, {binding}}}}).HasValue());
        static_cast<void>(fixture.Sample(std::nullopt, false));
        fixture.Begin(std::nullopt);
        fixture.input.Collector().SetModifiers({.control = true});
        static_cast<void>(fixture.input.CommitFrame());
        static_cast<void>(fixture.Read());
        fixture.Begin(std::nullopt);
        fixture.input.Collector().SetModifiers({});
        static_cast<void>(fixture.input.CommitFrame());
        static_cast<void>(fixture.Read());
        REQUIRE(fixture.Sample(std::nullopt, true).value.pressed);
        REQUIRE(fixture.Sample(std::nullopt, true).value.down);
        REQUIRE(fixture.Sample(std::nullopt, false).value.released);
        REQUIRE_FALSE(fixture.Sample(std::nullopt, true).value.down);
        REQUIRE(fixture.input.Router().Snapshot().State(Key::K).pressed);
        REQUIRE_FALSE(fixture.input.Router().Snapshot().modifiers.control);
    }

    TEST_CASE("Profile rejection preserves live state while accepted rebinding withdraws the previous mapping first",
              "[input][accessibility][router][profile]") {
        MappingFixture fixture{{.digital = {.toggle = true}}};
        static_cast<void>(fixture.Sample(std::nullopt, false));
        REQUIRE(fixture.Sample(std::nullopt, true).value.down);
        static_cast<void>(fixture.Sample(std::nullopt, false));
        InputBinding replacement{.kind = BindingControlKind::Key, .key = Key::L};
        replacement.accessibility.digital.toggle = true;
        replacement.accessibility.digital.repeatIntervalMicroseconds = 0;
        REQUIRE(fixture.input.Router().SetProfile({.overrides = {{fixture.action, {replacement}}}}).HasError());
        REQUIRE(fixture.Read().value.down);
        replacement.accessibility.digital.repeatIntervalMicroseconds = 100000;
        REQUIRE(fixture.input.Router().SetProfile({.overrides = {{fixture.action, {replacement}}}}).HasValue());
        REQUIRE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.down);
        fixture.Begin(std::nullopt);
        fixture.input.Collector().SetKey(Key::L, true);
        static_cast<void>(fixture.input.CommitFrame());
        REQUIRE(fixture.Read().value.pressed);
        REQUIRE(fixture.Read().value.down);
    }

    TEST_CASE("Device inventory changes discard a sequential modifier latch before another physical binding can use it",
              "[input][accessibility][router][sticky][device]") {
        MappingFixture fixture{{.stickyModifiers = true}};
        InputBinding binding{.kind = BindingControlKind::Key, .key = Key::K, .requiredModifiers = {.control = true}};
        binding.accessibility.stickyModifiers = true;
        REQUIRE(fixture.input.Router().SetProfile({.overrides = {{fixture.action, {binding}}}}).HasValue());
        static_cast<void>(fixture.Sample(std::nullopt, false));
        fixture.Begin(std::nullopt);
        fixture.input.Collector().SetModifiers({.control = true});
        static_cast<void>(fixture.input.CommitFrame());
        fixture.Begin(std::nullopt);
        fixture.input.Collector().SetModifiers({});
        static_cast<void>(fixture.input.CommitFrame());
        fixture.Begin(std::nullopt);
        static_cast<void>(fixture.input.Collector().ConnectGamepad("New device"));
        fixture.input.Collector().SetKey(Key::K, true);
        static_cast<void>(fixture.input.CommitFrame());
        REQUIRE_FALSE(fixture.Read().value.down);
        REQUIRE(fixture.input.Router().Snapshot().State(Key::K).pressed);
    }

    TEST_CASE("Next sample reclaims drained mapping identities for hosts without an explicit frame-end call",
              "[input][accessibility][router][lifetime][capacity]") {
        MappingFixture fixture{{.digital = {.toggle = true}}};
        for (std::size_t index = 0; index < MaximumAccessibilityMappings + 1; ++index) {
            REQUIRE(fixture.input.Router().SetProfile({}).HasValue());
            fixture.input.BeginFrame(++fixture.frame);
            fixture.input.Collector().SetKey(Key::K, false);
            static_cast<void>(fixture.input.CommitFrame());
            REQUIRE(fixture.Read().status == ActionReadStatus::Resolved);
            fixture.input.BeginFrame(++fixture.frame);
            fixture.input.Collector().SetKey(Key::K, true);
            static_cast<void>(fixture.input.CommitFrame());
            REQUIRE(fixture.Read().value.pressed);
            REQUIRE(fixture.input.Router().SetProfile({}).HasValue());
            REQUIRE(fixture.Read().value.released);
        }
    }

    TEST_CASE("Host input cutoffs reclaim ended context slots without stealing live cancellation releases",
              "[input][accessibility][router][withdrawal][capacity]") {
        MappingFixture fixture{{.digital = {.toggle = true}}};
        const auto sample = [&](const bool down) {
            fixture.input.BeginFrame(++fixture.frame);
            fixture.input.Collector().SetKey(Key::K, down);
            static_cast<void>(fixture.input.CommitFrame());
            return fixture.Read();
        };
        for (std::size_t index = 0; index < MaximumAccessibilityMappings + 1; ++index) {
            static_cast<void>(sample(false));
            const auto pressed = sample(true);
            REQUIRE(pressed.status == ActionReadStatus::Resolved);
            REQUIRE(pressed.value.down);
            const auto oldIdentity = fixture.input.Router().RoutingState(fixture.context).contextIdentity;
            fixture.context.Reset();
            fixture.context = fixture.input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
            // The production host drains after committing the next snapshot, without calling a removed owner.
            fixture.input.BeginFrame(++fixture.frame);
            static_cast<void>(fixture.input.CommitFrame());
            RetiredInputActionRelease retired;
            REQUIRE(fixture.input.Router().TryDequeueRetiredActionRelease(retired));
            REQUIRE(retired.contextIdentity == oldIdentity);
            REQUIRE(retired.action);
            REQUIRE(*retired.action == fixture.action);
            REQUIRE_FALSE(fixture.input.Router().TryDequeueRetiredActionRelease(retired));
            REQUIRE_FALSE(fixture.Read().value.released);
            REQUIRE_FALSE(fixture.Read().value.down);
        }
        static_cast<void>(sample(false));
        REQUIRE(sample(true).value.down);
        auto modal = fixture.input.Router().PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        RetiredInputActionRelease retired;
        REQUIRE_FALSE(fixture.input.Router().TryDequeueRetiredActionRelease(retired));
        REQUIRE(fixture.Read().value.released);
        REQUIRE_FALSE(fixture.Read().value.down);
    }

    TEST_CASE("Assigned device changes retain old-player release ownership and require fresh input for the new player",
              "[input][accessibility][router][device]") {
        InputService input;
        VirtualGamepad pad{input.Collector()};
        InputBinding binding{.kind = BindingControlKind::GamepadButton, .gamepadButton = GamepadButton::South};
        binding.accessibility.digital.toggle = true;
        const ActionId action{"play.device"};
        REQUIRE(input.Router().SetActionMap({{action, ActionValueType::Digital, InputContextId{"play"}, true, {binding}}}).HasValue());
        auto context = input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
        input.BeginFrame(1);
        const auto oldDevice = pad.Connect();
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().AssignGamepad(0, oldDevice));
        REQUIRE_FALSE(input.Router().ReadAction(context, action, 0).down);
        input.Router().EndFrame();
        input.BeginFrame(2);
        REQUIRE(pad.Press(GamepadButton::South));
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().ReadAction(context, action, 0).down);
        REQUIRE(input.Router().AssignGamepad(1, oldDevice));
        REQUIRE_FALSE(input.Router().ReadAction(context, action).released);
        REQUIRE(input.Router().ReadAction(context, action, 0).released);
        REQUIRE_FALSE(input.Router().ReadAction(context, action, 1).down);
        input.Router().EndFrame();
        input.BeginFrame(3);
        REQUIRE(pad.Release(GamepadButton::South));
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().ReadAction(context, action, 1).down);
        input.Router().EndFrame();
        input.BeginFrame(4);
        REQUIRE(pad.Press(GamepadButton::South));
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().ReadAction(context, action, 1).down);
        input.Router().EndFrame();
        input.BeginFrame(5);
        pad.Disconnect();
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().ReadAction(context, action, 1).released);
        REQUIRE_FALSE(input.Router().ReadAction(context, action, 1).down);
        input.Router().EndFrame();
        input.BeginFrame(6);
        const auto replacement = pad.Connect();
        REQUIRE(replacement != oldDevice);
        REQUIRE(pad.Press(GamepadButton::South));
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().AssignGamepad(1, replacement));
        REQUIRE_FALSE(input.Router().ReadAction(context, action, 1).down);
        input.Router().EndFrame();
        input.BeginFrame(7);
        REQUIRE(pad.Release(GamepadButton::South));
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().ReadAction(context, action, 1).down);
        input.Router().EndFrame();
        input.BeginFrame(8);
        REQUIRE(pad.Press(GamepadButton::South));
        static_cast<void>(input.CommitFrame());
        const auto fresh = input.Router().ReadActionEvidence(context, action, 1);
        REQUIRE(fresh.value.down);
        REQUIRE(fresh.source.has_value());
        REQUIRE(fresh.source->gamepad == replacement);
    }

    TEST_CASE("Mapping capacity refusal rolls back consumed raw edges and every uncommitted candidate",
              "[input][accessibility][router][capacity]") {
        InputService input;
        std::vector<ActionDescriptor> actions;
        for (std::size_t index = 0; index < MaximumAccessibilityMappings - 1; ++index) {
            InputBinding binding{.kind = BindingControlKind::RawGamepadButton, .rawControl = static_cast<std::uint16_t>(index % 256)};
            const auto modifier = index / 256;
            binding.requiredModifiers = {.control = (modifier & 1) != 0, .shift = (modifier & 2) != 0};
            binding.accessibility.digital.toggle = true;
            actions.push_back(
                {ActionId{"play.capacity." + std::to_string(index)}, ActionValueType::Digital, InputContextId{"play"}, false, {binding}});
        }
        InputBinding first{.kind = BindingControlKind::Key, .key = Key::K};
        first.accessibility.digital.toggle = true;
        auto second = first;
        second.key = Key::L;
        const ActionId target{"play.target"};
        actions.push_back({target, ActionValueType::Digital, InputContextId{"play"}, false, {first, second}});
        REQUIRE(input.Router().SetActionMap(std::move(actions)).HasValue());
        auto old = input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
        input.BeginFrame(1);
        const auto device = input.Collector().ConnectGamepad("Capacity fixture", false, 256);
        static_cast<void>(input.CommitFrame());
        for (std::size_t index = 0; index < MaximumAccessibilityMappings - 1; ++index)
            REQUIRE(input.Router().ReadActionEvidence(old, input.Router().Actions()[index].id).status == ActionReadStatus::Resolved);
        for (std::size_t group = 0; group < 4; ++group) {
            input.BeginFrame(group + 2);
            input.Collector().SetModifiers({.control = (group & 1) != 0, .shift = (group & 2) != 0});
            for (std::size_t button = 0; button < 256; ++button) {
                REQUIRE(input.Collector().SetRawGamepadButton(device, button, false));
                REQUIRE(input.Collector().SetRawGamepadButton(device, button, true));
            }
            static_cast<void>(input.CommitFrame());
            for (std::size_t index = 0; index < MaximumAccessibilityMappings - 1; ++index)
                static_cast<void>(input.Router().ReadAction(old, input.Router().Actions()[index].id));
        }
        auto next = input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
        REQUIRE(input.Router().ReadActionEvidence(next, target).status == ActionReadStatus::CapacityExceeded);
        GameplayInputFrameBuilder frames{{}, {}, target, {}};
        frames.Capture(input.Router(), next);
        REQUIRE(frames.LastCaptureStatus() == ActionReadStatus::CapacityExceeded);
        REQUIRE(frames.Consume(1) == GameplayInputFrame{.tick = 1});
        input.BeginFrame(6);
        input.Collector().SetModifiers({});
        input.Collector().SetKey(Key::K, true);
        input.Collector().SetKey(Key::L, true);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().ReadActionEvidence(next, target).status == ActionReadStatus::CapacityExceeded);
        REQUIRE(input.Router().ConsumeKey(next, Key::K));
        REQUIRE(input.Router().ConsumeKey(next, Key::L));
        for (std::size_t index = 0; index < MaximumAccessibilityMappings - 1; ++index)
            REQUIRE(input.Router().ReadAction(old, input.Router().Actions()[index].id).released);
        input.Router().EndFrame();
        input.BeginFrame(7);
        static_cast<void>(input.CommitFrame());
        const auto afterCleanup = input.Router().ReadActionEvidence(next, target);
        REQUIRE(afterCleanup.status == ActionReadStatus::Resolved);
        REQUIRE_FALSE(afterCleanup.value.down);
        REQUIRE_FALSE(afterCleanup.value.pressed);
    }
}  // namespace Horo::Input
