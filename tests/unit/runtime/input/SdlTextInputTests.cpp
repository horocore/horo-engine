#include "SdlInputBackend.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Input;

    TEST_CASE("SDL committed text and editing events share the bounded snapshot pipeline", "[unit][runtime][input][sdl][text]") {
        SdlInputBackend backend;
        backend.BindWindow(7);
        backend.BeginFrame(1);
        SDL_Event editing{};
        editing.type = SDL_EVENT_TEXT_EDITING;
        editing.edit.windowID = 7;
        char preedit[] = "preedit";
        editing.edit.text = preedit;
        editing.edit.start = 1;
        editing.edit.length = 2;
        backend.ProcessEvent(editing);
        const RawInputSnapshot &first = backend.Commit();
        REQUIRE(first.composition.active);
        REQUIRE(first.composition.text == "preedit");
        REQUIRE(first.composition.selectionStart == 1);

        backend.BeginFrame(2);
        SDL_Event committed{};
        committed.type = SDL_EVENT_TEXT_INPUT;
        committed.text.windowID = 7;
        char committedText[] = "\xC3\xB6";
        committed.text.text = committedText;
        backend.ProcessEvent(committed);
        const RawInputSnapshot &second = backend.Commit();
        REQUIRE(second.text == "\xC3\xB6");
        REQUIRE_FALSE(second.composition.active);
        REQUIRE(second.compositionRevision > first.compositionRevision);
    }

    TEST_CASE("SDL text from a different window cannot enter the focused snapshot", "[unit][runtime][input][sdl][text]") {
        SdlInputBackend backend;
        backend.BindWindow(7);
        backend.BeginFrame(1);
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.windowID = 8;
        event.text.text = "foreign";
        backend.ProcessEvent(event);
        SDL_Event foreignKey{};
        foreignKey.type = SDL_EVENT_KEY_DOWN;
        foreignKey.key.windowID = 8;
        foreignKey.key.scancode = SDL_SCANCODE_A;
        foreignKey.key.down = true;
        backend.ProcessEvent(foreignKey);
        const RawInputSnapshot &foreign = backend.Commit();
        REQUIRE(foreign.text.empty());
        REQUIRE_FALSE(foreign.State(Key::A).down);

        backend.BeginFrame(2);
        SDL_Event ownKey = foreignKey;
        ownKey.key.windowID = 7;
        backend.ProcessEvent(ownKey);
        const RawInputSnapshot &owned = backend.Commit();
        REQUIRE(owned.State(Key::A).pressed);

        backend.BindWindow(9);
        backend.BeginFrame(3);
        const RawInputSnapshot &rebound = backend.Commit();
        REQUIRE_FALSE(rebound.State(Key::A).down);
        REQUIRE(rebound.State(Key::A).released);
    }

    TEST_CASE("SDL candidate area hook rejects missing windows and invalid geometry", "[unit][runtime][input][sdl][text]") {
        SdlInputBackend backend;
        REQUIRE_FALSE(backend.StartTextInput(nullptr, {0, 0, 1, 1}));
        REQUIRE_FALSE(backend.SetTextInputArea(nullptr, {0, 0, 1, 1}));
        backend.StopTextInput(nullptr);
    }
}  // namespace
