#include "Horo/Runtime/Input.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    using namespace Horo::Input;

    TEST_CASE("Text is delivered only once to the explicitly focused context", "[unit][runtime][input][text]") {
        InputService input;
        auto gameplay = input.Router().PushContext(InputContextId{"gameplay"}, InputContextKind::Gameplay);
        REQUIRE_FALSE(input.Router().FocusText(gameplay));
        auto widget = input.Router().PushContext(InputContextId{"editor.widget"}, InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(widget));

        input.BeginFrame(1);
        input.Collector().AppendText("first");
        input.Collector().AppendText(" frame");
        static_cast<void>(input.CommitFrame());
        const auto first = input.Router().TakeText(widget);
        REQUIRE(first.has_value());
        REQUIRE(first->committed == "first frame");
        REQUIRE_FALSE(input.Router().TakeText(widget).has_value());
        REQUIRE_FALSE(input.Router().TakeText(gameplay).has_value());

        input.Router().BlurText(widget);
        input.BeginFrame(2);
        input.Collector().AppendText("discarded");
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().TakeText(widget).has_value());
    }

    TEST_CASE("Focus transfer and modal boundaries discard stale committed text and pre-edit", "[unit][runtime][input][text]") {
        InputService input;
        auto first = input.Router().PushContext(InputContextId{"first"}, InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(first));
        input.BeginFrame(1);
        input.Collector().AppendText("a");
        input.Collector().SetTextComposition("preedit", 0, 7);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().TakeText(first)->composition.active);

        auto modal = input.Router().PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        REQUIRE_FALSE(input.Router().TakeText(first).has_value());
        REQUIRE(input.Router().FocusText(modal));
        REQUIRE_FALSE(input.Router().TakeText(modal).has_value());
        input.BeginFrame(2);
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().TakeText(modal).has_value());
        input.BeginFrame(3);
        input.Collector().SetTextComposition("new", 0, 3);
        static_cast<void>(input.CommitFrame());
        const auto current = input.Router().TakeText(modal);
        REQUIRE(current.has_value());
        REQUIRE(current->composition.text == "new");
        REQUIRE(current->compositionChanged);

        modal.Reset();
        REQUIRE_FALSE(input.Router().TakeText(first).has_value());
        input.BeginFrame(4);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().FocusText(first));
        REQUIRE_FALSE(input.Router().TakeText(first).has_value());
        input.BeginFrame(5);
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().TakeText(first).has_value());
    }

    TEST_CASE("Commit clears pre-edit and focus loss prevents replay", "[unit][runtime][input][text]") {
        InputService input;
        auto widget = input.Router().PushContext(InputContextId{"widget"}, InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(widget));
        input.BeginFrame(1);
        input.Collector().SetTextComposition("\xC3\xB6", 0, 1);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().TakeText(widget)->composition.active);

        input.BeginFrame(2);
        input.Collector().AppendText("\xC3\xB6");
        static_cast<void>(input.CommitFrame());
        const auto committed = input.Router().TakeText(widget);
        REQUIRE(committed.has_value());
        REQUIRE(committed->committed == "\xC3\xB6");
        REQUIRE_FALSE(committed->composition.active);
        REQUIRE(committed->compositionChanged);

        input.BeginFrame(3);
        input.Collector().SetWindowState({.focused = false});
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().TakeText(widget).has_value());
        input.BeginFrame(4);
        input.Collector().SetWindowState({.focused = true});
        input.Collector().AppendText("unowned");
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().TakeText(widget).has_value());
    }

    TEST_CASE("A newer text context cannot replay a peer's same-frame text", "[unit][runtime][input][text]") {
        InputService input;
        auto first = input.Router().PushContext(InputContextId{"first"}, InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(first));
        input.BeginFrame(1);
        input.Collector().AppendText("old");
        static_cast<void>(input.CommitFrame());
        auto second = input.Router().PushContext(InputContextId{"second"}, InputContextKind::FocusedGuiWidget);
        REQUIRE_FALSE(input.Router().TakeText(first).has_value());
        REQUIRE(input.Router().FocusText(second));
        REQUIRE_FALSE(input.Router().TakeText(second).has_value());
        input.BeginFrame(2);
        input.Collector().AppendText("new");
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().TakeText(second)->committed == "new");
    }

    TEST_CASE("Collector rejects malformed and oversized text without partial UTF-8 delivery", "[unit][runtime][input][text]") {
        InputService input;
        auto widget = input.Router().PushContext(InputContextId{"widget"}, InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(widget));
        input.BeginFrame(1);
        input.Collector().AppendText("ok");
        input.Collector().AppendText("\xC3");
        input.Collector().AppendText(std::string(4096, 'x'));
        input.Collector().SetTextComposition(std::string(4097, 'y'), 0, 4097);
        static_cast<void>(input.CommitFrame());
        const auto delivery = input.Router().TakeText(widget);
        REQUIRE(delivery.has_value());
        REQUIRE(delivery->committed == "ok");
        REQUIRE_FALSE(delivery->composition.active);

        input.BeginFrame(2);
        input.Collector().SetTextComposition("\xC3\xB6", 100, 100);
        const RawInputSnapshot &clamped = input.CommitFrame();
        REQUIRE(clamped.composition.selectionStart == 1);
        REQUIRE(clamped.composition.selectionLength == 0);
    }
}  // namespace
