#include "Horo/Runtime/Input.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Input;

    InputBinding KeyBinding(const Key key, const float scale = 1.0F, const std::uint8_t component = 0) {
        return InputBinding{.kind = BindingControlKind::Key, .key = key, .scale = scale, .component = component};
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
