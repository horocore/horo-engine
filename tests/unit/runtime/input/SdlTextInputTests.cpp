#include "SdlInputTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace {
    using namespace Horo::Input;

    /** @brief Records cancellation delivered by the central router. */
    struct PointerOwner final : IInputCaptureOwner {
        void OnInputCaptureCancelled(const CaptureCancellationReason value) noexcept override {
            reason = value;
        }
        std::optional<CaptureCancellationReason> reason;
    };

    TEST_CASE("SDL committed text and editing events share the bounded snapshot pipeline", "[unit][runtime][input][sdl][text]") {
        SdlInputBackend backend;
        Horo::Tests::BindFocusedSdlTestWindow(backend, 7);
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

    TEST_CASE("SDL keyboard and text require focus from the exact bound window across replacement", "[unit][runtime][input][sdl][text]") {
        SdlInputBackend backend;
        Horo::Tests::BindFocusedSdlTestWindow(backend, 7);
        backend.BeginFrame(1);
        const SDL_Event foreignText{.text = {.type = SDL_EVENT_TEXT_INPUT, .windowID = 8, .text = "foreign"}};
        backend.ProcessEvent(foreignText);
        const SDL_Event foreignKey{.key = {.type = SDL_EVENT_KEY_DOWN, .windowID = 8, .scancode = SDL_SCANCODE_A, .down = true}};
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
        Horo::Tests::SendSdlWindowEvent(backend, 7, SDL_EVENT_WINDOW_FOCUS_GAINED);
        const SDL_Event beforeFocus{.text = {.type = SDL_EVENT_TEXT_INPUT, .windowID = 9, .text = "before focus"}};
        backend.ProcessEvent(beforeFocus);
        ownKey.key.windowID = 9;
        ownKey.key.scancode = SDL_SCANCODE_B;
        backend.ProcessEvent(ownKey);
        const RawInputSnapshot &rebound = backend.Commit();
        CHECK_FALSE(rebound.window.focused);
        CHECK_FALSE(rebound.window.pointerInside);
        CHECK_FALSE(rebound.State(Key::A).down);
        CHECK(rebound.State(Key::A).released);
        CHECK_FALSE(rebound.State(Key::B).pressed);
        CHECK(rebound.text.empty());

        backend.BeginFrame(4);
        Horo::Tests::SendSdlWindowEvent(backend, 9, SDL_EVENT_WINDOW_FOCUS_GAINED);
        backend.ProcessEvent(ownKey);
        const SDL_Event afterFocus{.text = {.type = SDL_EVENT_TEXT_INPUT, .windowID = 9, .text = "after focus"}};
        backend.ProcessEvent(afterFocus);
        const RawInputSnapshot &focused = backend.Commit();
        CHECK(focused.window.focused);
        CHECK(focused.State(Key::B).pressed);
        CHECK(focused.text == "after focus");
    }

    TEST_CASE("SDL focus recovery publishes a neutral frame before accepting input", "[unit][runtime][input][sdl][focus-cutoff]") {
        SdlInputBackend backend;
        constexpr SDL_WindowID Window = 7;
        Horo::Tests::BindFocusedSdlTestWindow(backend, Window);
        backend.BeginFrame(1);
        const SDL_Event key{.key = {.type = SDL_EVENT_KEY_DOWN, .windowID = Window, .scancode = SDL_SCANCODE_A, .down = true}};
        backend.ProcessEvent(key);
        PointerOwner owner;
        InputRouter router;
        const auto context = router.PushContext(InputContextId{"workspace"}, InputContextKind::EditorWorkspace);
        router.BeginFrame(backend.Commit());
        const auto capture = router.CapturePointer(context, PointerButton::Primary, owner);
        REQUIRE(capture.HasValue());

        backend.BeginFrame(2);
        Horo::Tests::SendSdlWindowEvent(backend, Window, SDL_EVENT_WINDOW_FOCUS_LOST);
        Horo::Tests::SendSdlWindowEvent(backend, Window, SDL_EVENT_WINDOW_FOCUS_GAINED);
        SDL_Event text{.text = {.type = SDL_EVENT_TEXT_INPUT, .windowID = Window, .text = "queued text"}};
        backend.ProcessEvent(key);
        backend.ProcessEvent(text);
        const auto &neutral = backend.Commit();
        router.BeginFrame(neutral);
        CHECK_FALSE(neutral.window.focused);
        CHECK_FALSE(neutral.State(Key::A).down);
        CHECK(neutral.State(Key::A).released);
        CHECK(neutral.text.empty());
        CHECK(owner.reason == CaptureCancellationReason::FocusLost);
        CHECK_FALSE(router.HasCapture());

        backend.BeginFrame(3);
        backend.ProcessEvent(key);
        text.text.text = "fresh text";
        backend.ProcessEvent(text);
        const auto &recovered = backend.Commit();
        CHECK(recovered.window.focused);
        CHECK(recovered.State(Key::A).pressed);
        CHECK(recovered.text == "fresh text");
    }

    TEST_CASE("SDL candidate area hook rejects missing windows and invalid geometry", "[unit][runtime][input][sdl][text]") {
        SdlInputBackend backend;
        REQUIRE_FALSE(backend.StartTextInput(nullptr, {0, 0, 1, 1}));
        REQUIRE_FALSE(backend.SetTextInputArea(nullptr, {0, 0, 1, 1}));
        backend.StopTextInput(nullptr);
    }
}  // namespace

TEST_CASE("SDL punctuation keys retain their physical identity for configurable zoom chords", "[unit][runtime][input][sdl][keymap]") {
    using namespace Horo::Input;
    SdlInputBackend backend;
    Horo::Tests::BindFocusedSdlTestWindow(backend, 7);
    backend.BeginFrame(1);
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = 7;
    event.key.scancode = SDL_SCANCODE_EQUALS;
    event.key.down = true;
    event.key.mod = SDL_KMOD_GUI;
    backend.ProcessEvent(event);
    const auto &plus = backend.Commit();
    REQUIRE(plus.State(Key::Equal).pressed);
    REQUIRE(plus.modifiers.command);
    backend.BeginFrame(2);
    event.key.scancode = SDL_SCANCODE_MINUS;
    backend.ProcessEvent(event);
    REQUIRE(backend.Commit().State(Key::Minus).pressed);
}
