#include "Horo/Runtime/Input.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Input;

    InputBinding KeyBinding(const Key key, const float scale = 1.0F, const std::uint8_t component = 0) {
        return InputBinding{.kind = BindingControlKind::Key, .key = key, .scale = scale, .component = component};
    }

    TEST_CASE("Assigned gyro look rates reach fixed tick commands and recording without presentation integration",
              "[unit][runtime][input][gyroscope]") {
        InputRouter router;
        const ActionId look{"look"};
        const InputContextId play{"play"};
        const InputBinding yaw{.kind = BindingControlKind::GyroscopeAxis, .gyroscope = {SensorAxis::Y, 2, GyroscopeSource::Gamepad}};
        const InputBinding pitch{.kind = BindingControlKind::GyroscopeAxis,
                                 .scale = -1,
                                 .component = 1,
                                 .gyroscope = {SensorAxis::X, 4, GyroscopeSource::Gamepad}};
        REQUIRE(router.SetActionMap({{look, ActionValueType::Axis2D, play, true, {yaw, pitch}}}).HasValue());
        auto context = router.PushContext(play, InputContextKind::Gameplay);
        RawInputCollector collector;
        collector.BeginFrame(900);
        VirtualGamepad controller{collector};
        const auto id = controller.Connect();
        REQUIRE(controller.SetGyroscopeSample({SensorAvailability::Available, {1, 1, 0}, 0, 1}));
        router.BeginFrame(collector.Commit());
        REQUIRE(router.AssignGamepad(0, id));
        GameplayInputFrameBuilder builder{{}, look, {}, {}};
        builder.Capture(router, context, 0);
        builder.Capture(router, context, 0);
        const auto first = builder.Consume(10), catchUp = builder.Consume(11);
        CHECK(first.lookX == 0.5F);
        CHECK(first.lookY == -0.25F);
        CHECK(catchUp.lookX == first.lookX);
        CHECK(catchUp.lookY == first.lookY);
        GameplayInputRecording recording;
        recording.Record(first);
        recording.Record(catchUp);
        CHECK(recording.Next() == first);
        CHECK(recording.Next() == catchUp);
        collector.BeginFrame(901);
        REQUIRE(controller.SetGyroscopeSample({.availability = SensorAvailability::Disabled}));
        router.BeginFrame(collector.Commit());
        builder.Capture(router, context, 0);
        const auto stopped = builder.Consume(12);
        CHECK(stopped.lookX == 0);
        CHECK(stopped.lookY == 0);
        CHECK(builder.LastCaptureStatus() == ActionReadStatus::Resolved);
    }

    std::array<GameplayInputFrame, 3> ProduceCadence(const bool catchUp) {
        RawInputCollector collector;
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
        auto gameplay = router.PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, ActionId{"look"}, ActionId{"jump"}, ActionId{"interact"}};
        std::array<GameplayInputFrame, 3> commands{};

        collector.BeginFrame(1);
        collector.SetKey(Key::W, true);
        collector.SetKey(Key::Space, true);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        builder.Capture(router, gameplay);  // One committed snapshot cannot duplicate an edge.
        if (!catchUp)
            commands[0] = builder.Consume(40);

        collector.BeginFrame(2);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        if (catchUp)
            commands[0] = builder.Consume(40);
        commands[1] = builder.Consume(41);

        collector.BeginFrame(3);
        collector.SetKey(Key::W, false);
        collector.SetKey(Key::Space, false);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        commands[2] = builder.Consume(42);
        return commands;
    }

    TEST_CASE("Gameplay Commands Survive Zero Tick Frames And Match Different Presentation Cadences", "[unit][runtime][input]") {
        const auto steady = ProduceCadence(false);
        const auto catchUp = ProduceCadence(true);
        REQUIRE((steady == catchUp));
        REQUIRE((steady[0].tick == 40 && steady[0].moveY == 1.0F && steady[0].jumpPressed));
        REQUIRE((steady[0].moveDown && steady[0].movePressed && !steady[0].moveReleased));
        REQUIRE((steady[1].tick == 41 && steady[1].moveY == 1.0F && !steady[1].jumpPressed));
        REQUIRE((steady[1].moveDown && !steady[1].movePressed && !steady[1].moveReleased));
        REQUIRE((steady[2].tick == 42 && steady[2].moveY == 0.0F && !steady[2].jumpPressed));
        REQUIRE((!steady[2].moveDown && !steady[2].movePressed && steady[2].moveReleased));

        GameplayInputRecording recording;
        for (const GameplayInputFrame &command : steady)
            recording.Record(command);
        for (const GameplayInputFrame &command : steady)
            REQUIRE((recording.Next() == command));
        REQUIRE((!recording.Next().has_value()));
        recording.ResetReplay();
        for (const GameplayInputFrame &command : catchUp)
            REQUIRE((recording.Next() == command));
    }

    TEST_CASE("Gameplay Repeat Commands Survive Zero Tick Frames And Fire Once", "[unit][runtime][input][accessibility]") {
        InputService input;
        const InputContextId gameplayId{"gameplay"};
        auto jumpBinding = KeyBinding(Key::Space);
        auto interactBinding = KeyBinding(Key::K);
        jumpBinding.accessibility.digital = {.repeat = true, .repeatDelayMicroseconds = 50, .repeatIntervalMicroseconds = 20};
        interactBinding.accessibility = jumpBinding.accessibility;
        REQUIRE(input.Router()
                    .SetActionMap({{ActionId{"jump"}, ActionValueType::Digital, gameplayId, true, {jumpBinding}},
                                   {ActionId{"interact"}, ActionValueType::Digital, gameplayId, true, {interactBinding}}})
                    .HasValue());
        auto gameplay = input.Router().PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, ActionId{"look"}, ActionId{"jump"}, ActionId{"interact"}};
        FrameNumber frame{};
        const auto capture = [&](const std::uint64_t time, const bool down, const bool focused = true) {
            input.Router().EndFrame();
            input.BeginFrame(++frame, InputSampleTime{time});
            input.Collector().SetKey(Key::Space, down);
            input.Collector().SetKey(Key::K, down);
            input.Collector().SetWindowState({.focused = focused});
            static_cast<void>(input.CommitFrame());
            builder.Capture(input.Router(), gameplay);
        };
        capture(0, false);
        capture(10, true);
        const auto activation = builder.Consume(1);
        REQUIRE(activation.jumpPressed);
        REQUIRE(activation.interactPressed);
        REQUIRE(builder.Consume(2) == GameplayInputFrame{.tick = 2});

        capture(60, true);  // Repeat, followed by a presentation frame without a repeat.
        builder.Capture(input.Router(), gameplay);
        capture(61, true);
        const auto repeat = builder.Consume(3);
        REQUIRE(repeat.jumpPressed);
        REQUIRE(repeat.interactPressed);
        REQUIRE(builder.Consume(4) == GameplayInputFrame{.tick = 4});

        capture(80, true);
        capture(100, true);  // Two repeats coalesce before the next fixed tick.
        REQUIRE(builder.Consume(5).jumpPressed);
        REQUIRE(builder.Consume(6) == GameplayInputFrame{.tick = 6});
        capture(120, true);
        capture(121, true, false);  // Losing ownership clears an unconsumed repeat.
        REQUIRE(builder.Consume(7) == GameplayInputFrame{.tick = 7});
        capture(130, false);
        capture(140, true);
        builder.Reset();
        REQUIRE(builder.Consume(8) == GameplayInputFrame{.tick = 8});
    }

    TEST_CASE("Gameplay Capture Preserves Clock Failures And Clears Previously Latched Commands", "[unit][runtime][input][accessibility]") {
        InputService input;
        const InputContextId gameplayId{"gameplay"};
        auto jumpBinding = KeyBinding(Key::Space);
        jumpBinding.accessibility.digital.holdMicroseconds = 10;
        REQUIRE(input.Router()
                    .SetActionMap({{ActionId{"move"}, ActionValueType::Axis2D, gameplayId, true, {KeyBinding(Key::W, 1.0F, 1)}},
                                   {ActionId{"jump"}, ActionValueType::Digital, gameplayId, true, {jumpBinding}},
                                   {ActionId{"interact"}, ActionValueType::Digital, gameplayId, true, {KeyBinding(Key::K)}}})
                    .HasValue());
        auto gameplay = input.Router().PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, ActionId{"look"}, ActionId{"jump"}, ActionId{"interact"}};
        REQUIRE(builder.LastCaptureStatus() == ActionReadStatus::Unavailable);
        FrameNumber frame{};
        const auto capture = [&](const std::optional<InputSampleTime> time, const bool down) {
            input.BeginFrame(++frame, time);
            input.Collector().SetKey(Key::W, down);
            input.Collector().SetKey(Key::Space, down);
            input.Collector().SetKey(Key::K, down);
            static_cast<void>(input.CommitFrame());
            builder.Capture(input.Router(), gameplay);
        };
        capture(InputSampleTime{0}, false);
        capture(InputSampleTime{1}, true);
        capture(InputSampleTime{11}, true);  // Movement and two commands are pending without a fixed tick.
        REQUIRE(builder.LastCaptureStatus() == ActionReadStatus::Resolved);
        capture(std::nullopt, true);
        REQUIRE(builder.LastCaptureStatus() == ActionReadStatus::ClockUnavailable);
        builder.Capture(input.Router(), gameplay);
        REQUIRE(builder.LastCaptureStatus() == ActionReadStatus::ClockUnavailable);
        REQUIRE(builder.Consume(1) == GameplayInputFrame{.tick = 1});

        capture(InputSampleTime{20}, true);
        REQUIRE(builder.LastCaptureStatus() == ActionReadStatus::Resolved);
        const auto restored = builder.Consume(2);
        REQUIRE(restored.moveY == 1.0F);
        REQUIRE_FALSE(restored.jumpPressed);
        REQUIRE_FALSE(restored.interactPressed);
        capture(InputSampleTime{21}, false);
        capture(InputSampleTime{22}, true);
        const auto fresh = builder.Consume(3);
        REQUIRE_FALSE(fresh.jumpPressed);
        REQUIRE(fresh.interactPressed);
        capture(InputSampleTime{32}, true);
        REQUIRE(builder.Consume(4).jumpPressed);
        builder.Reset();
        REQUIRE(builder.LastCaptureStatus() == ActionReadStatus::Unavailable);
        REQUIRE(builder.Consume(5) == GameplayInputFrame{.tick = 5});
    }

    TEST_CASE("Gameplay Capture Neutralizes Pending Commands When Ownership Changes", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const InputContextId gameplayId{"gameplay"};
        REQUIRE((
            router
                .SetActionMap({ActionDescriptor{ActionId{"move"}, ActionValueType::Axis2D, gameplayId, true, {KeyBinding(Key::W, 1.0F, 1)}},
                               ActionDescriptor{ActionId{"jump"}, ActionValueType::Digital, gameplayId, true, {KeyBinding(Key::Space)}}})
                .HasValue()));
        auto gameplay = router.PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, ActionId{"look"}, ActionId{"jump"}, ActionId{"interact"}};
        collector.BeginFrame(1);
        collector.SetKey(Key::W, true);
        collector.SetKey(Key::Space, true);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);  // No simulation tick yet.

        collector.BeginFrame(2);
        router.BeginFrame(collector.Commit());
        auto modal = router.PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        builder.Capture(router, gameplay);
        REQUIRE((builder.Consume(1) == GameplayInputFrame{.tick = 1}));

        modal.Reset();
        collector.BeginFrame(3);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        REQUIRE((builder.Consume(2).moveY == 1.0F));
        REQUIRE((!builder.Consume(3).jumpPressed));

        collector.BeginFrame(4);
        collector.SetWindowState({.focused = false});
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        REQUIRE((builder.Consume(4) == GameplayInputFrame{.tick = 4}));
        builder.Reset();
        REQUIRE((builder.Consume(5) == GameplayInputFrame{.tick = 5}));
    }

    TEST_CASE("Gameplay Capture Uses The Consumption Ledger Before Tick Production", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const InputContextId gameplayId{"gameplay"};
        REQUIRE(
            (router.SetActionMap({ActionDescriptor{ActionId{"jump"}, ActionValueType::Digital, gameplayId, true, {KeyBinding(Key::Space)}}})
                 .HasValue()));
        auto gameplay = router.PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, ActionId{"look"}, ActionId{"jump"}, ActionId{"interact"}};
        collector.BeginFrame(1);
        collector.SetKey(Key::Space, true);
        router.BeginFrame(collector.Commit());
        auto gui = router.PushContext(InputContextId{"gui"}, InputContextKind::FocusedGuiWidget);
        REQUIRE((router.ConsumeKey(gui, Key::Space)));
        gui.Reset();
        builder.Capture(router, gameplay);
        REQUIRE((!builder.Consume(1).jumpPressed));

        collector.BeginFrame(2);
        collector.SetKey(Key::Space, false);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        REQUIRE((!builder.Consume(2).jumpPressed));

        collector.BeginFrame(3);
        collector.SetKey(Key::Space, true);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        REQUIRE((builder.Consume(3).jumpPressed));
    }

    TEST_CASE("Tick Frames Preserve Move Action Edges When Opposing Axes Cancel", "[unit][runtime][input]") {
        RawInputCollector collector;
        InputRouter router;
        const InputContextId gameplayId{"gameplay"};
        REQUIRE((router
                     .SetActionMap({ActionDescriptor{ActionId{"move"},
                                                     ActionValueType::Axis2D,
                                                     gameplayId,
                                                     true,
                                                     {KeyBinding(Key::W, 1.0F, 1), KeyBinding(Key::S, -1.0F, 1)}}})
                     .HasValue()));
        auto gameplay = router.PushContext(gameplayId, InputContextKind::Gameplay);
        GameplayInputFrameBuilder builder{ActionId{"move"}, {}, {}, {}};

        collector.BeginFrame(1);
        collector.SetKey(Key::W, true);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        const GameplayInputFrame first = builder.Consume(1);
        REQUIRE((first.moveY == 1.0F && first.moveDown && first.movePressed && !first.moveReleased));

        collector.BeginFrame(2);
        collector.SetKey(Key::S, true);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        const GameplayInputFrame opposed = builder.Consume(2);
        REQUIRE((opposed.moveY == 0.0F && opposed.moveDown && opposed.movePressed && !opposed.moveReleased));
        REQUIRE((!builder.Consume(3).movePressed));

        collector.BeginFrame(3);
        collector.SetKey(Key::W, false);
        router.BeginFrame(collector.Commit());
        builder.Capture(router, gameplay);
        const GameplayInputFrame partialRelease = builder.Consume(4);
        REQUIRE((partialRelease.moveY == -1.0F && partialRelease.moveDown && partialRelease.moveReleased));
    }
}  // namespace
