#include "UiTextInputAdapter.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;

    template <typename Id> Id AuthoredId(const std::uint8_t marker) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return Id::Create(bytes).Value();
    }

    UiActionOwnerContext Owner() {
        const auto generation = UiOwnershipGeneration::Create(73).Value();
        return {{generation, 1, 1},
                {generation, 2, 1},
                AuthoredId<UiDocumentId>(1),
                UiDocumentRevision::Create(1).Value(),
                UiRuntimeTreeRevision::Create(2).Value(),
                UiInteractionRevision::Create(3).Value()};
    }

    UiActionSource Source(const std::uint32_t slot) {
        const UiActionOwnerContext owner = Owner();
        return {owner, {owner.instance.ownership, slot, 1}};
    }

    UiControlStateMachine TextControl(const UiActionSource &source) {
        const UiControlDescriptorBase base{source.owner, source.element, AuthoredId<UiActionId>(9), {}, true, true, {}};
        const auto descriptor = UiTextInputControlDescriptor{base, UiActionText::Create("").Value(), 32, true};
        auto created = UiControlStateMachine::Create(descriptor);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    void Focus(UiControlStateMachine &control, const UiActionSource &source) {
        const UiControlInput focus{source, UiControlInputKind::FocusGained, UiControlActivationSource::Keyboard, 1};
        REQUIRE(control.Handle(focus).HasValue());
    }

    std::string Text(const UiControlStateMachine &control) {
        const auto snapshot = control.Snapshot();
        REQUIRE(snapshot.HasValue());
        return std::string(std::get<UiTextInputControlState>(snapshot.Value()).text.View());
    }

    TEST_CASE("Committed snapshot text reaches exactly one focused Runtime UI control", "[unit][runtime][input][text][ui]") {
        Input::InputService input;
        auto gameplay = input.Router().PushContext(Input::InputContextId{"gameplay"}, Input::InputContextKind::Gameplay);
        auto ui = input.Router().PushContext(Input::InputContextId{"runtime.ui.text"}, Input::InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(ui));
        auto firstSource = Source(3);
        auto secondSource = Source(4);
        auto first = TextControl(firstSource);
        auto second = TextControl(secondSource);
        Focus(first, firstSource);

        input.BeginFrame(1);
        input.Collector().AppendText("hello");
        static_cast<void>(input.CommitFrame());
        REQUIRE(InputAdapter::DeliverFocusedText(input.Router(), ui, first, secondSource, 2).HasError());
        const auto blockedGameplay = InputAdapter::DeliverFocusedText(input.Router(), gameplay, first, firstSource, 2);
        REQUIRE(blockedGameplay.HasValue());
        REQUIRE_FALSE(blockedGameplay.Value().has_value());
        const auto blockedUnfocused = InputAdapter::DeliverFocusedText(input.Router(), ui, second, secondSource, 2);
        REQUIRE(blockedUnfocused.HasValue());
        REQUIRE_FALSE(blockedUnfocused.Value().has_value());
        const auto delivered = InputAdapter::DeliverFocusedText(input.Router(), ui, first, firstSource, 2);
        REQUIRE(delivered.HasValue());
        REQUIRE(delivered.Value().has_value());
        REQUIRE(delivered.Value()->committed == "hello");
        REQUIRE(Text(first) == "hello");
        REQUIRE(Text(second).empty());
        REQUIRE_FALSE(InputAdapter::DeliverFocusedText(input.Router(), ui, first, firstSource, 3).Value().has_value());
    }

    TEST_CASE("Modal transition cancels previous UI pre-edit without replaying it to the new control", "[unit][runtime][input][text][ui]") {
        Input::InputService input;
        auto ui = input.Router().PushContext(Input::InputContextId{"runtime.ui.text"}, Input::InputContextKind::FocusedGuiWidget);
        REQUIRE(input.Router().FocusText(ui));
        auto firstSource = Source(3);
        auto modalSource = Source(4);
        auto first = TextControl(firstSource);
        auto modalControl = TextControl(modalSource);
        Focus(first, firstSource);
        Focus(modalControl, modalSource);

        input.BeginFrame(1);
        input.Collector().SetTextComposition("old preedit", 0, 3);
        static_cast<void>(input.CommitFrame());
        const auto oldPreedit = InputAdapter::DeliverFocusedText(input.Router(), ui, first, firstSource, 2);
        REQUIRE(oldPreedit.HasValue());
        REQUIRE(oldPreedit.Value()->composition.text == "old preedit");

        auto modal = input.Router().PushContext(Input::InputContextId{"runtime.ui.modal"}, Input::InputContextKind::ModalRoot);
        REQUIRE(input.Router().FocusText(modal));
        REQUIRE_FALSE(InputAdapter::DeliverFocusedText(input.Router(), ui, first, firstSource, 3).Value().has_value());
        input.BeginFrame(2);
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(InputAdapter::DeliverFocusedText(input.Router(), modal, modalControl, modalSource, 2).Value().has_value());

        input.BeginFrame(3);
        input.Collector().SetTextComposition("new preedit", 0, 3);
        static_cast<void>(input.CommitFrame());
        const auto newPreedit = InputAdapter::DeliverFocusedText(input.Router(), modal, modalControl, modalSource, 2);
        REQUIRE(newPreedit.HasValue());
        REQUIRE(newPreedit.Value()->composition.text == "new preedit");
        REQUIRE(Text(first).empty());
        REQUIRE(Text(modalControl).empty());

        input.BeginFrame(4);
        input.Collector().AppendText("new");
        static_cast<void>(input.CommitFrame());
        REQUIRE(InputAdapter::DeliverFocusedText(input.Router(), modal, modalControl, modalSource, 2).HasValue());
        REQUIRE(Text(first).empty());
        REQUIRE(Text(modalControl) == "new");
    }
}  // namespace
