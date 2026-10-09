#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Horo::Runtime::Ui {
    namespace {
        UiActionText Text(std::string_view text) {
            return UiActionText::Create(text).Value();
        }

        UiTextEditBuffer Buffer(std::string_view text = {}, const UiTextEditPolicy &policy = {}) {
            auto result = UiTextEditBuffer::Create(policy, Text(text));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        template <typename Id> Id Authored(std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        UiControlDescriptorBase Base() {
            const auto owner = UiOwnershipGeneration::Create(722).Value();
            UiActionOwnerContext context{{owner, 1, 1},
                                         {owner, 2, 1},
                                         Authored<UiDocumentId>(1),
                                         UiDocumentRevision::Create(1).Value(),
                                         UiRuntimeTreeRevision::Create(2).Value(),
                                         UiInteractionRevision::Create(3).Value()};
            return {context, {owner, 3, 1}, Authored<UiActionId>(4), {}, true, true, {}};
        }

        UiControlStateMachine Control(std::string_view initial, UiTextEditPolicy policy = {}) {
            const UiTextInputEditingOptions options{policy.maximumGraphemes, policy.undoDepth, policy.validation, policy.password};
            return std::move(UiControlStateMachine::Create(
                                 UiTextInputControlDescriptor{Base(), Text(initial), policy.maximumBytes, true, options}))
                .Value();
        }

        UiControlInput Input(UiControlInputKind kind, std::uint64_t sequence, UiActionText text = {}) {
            const auto base = Base();
            return {{base.owner, base.element}, kind, UiControlActivationSource::Keyboard, sequence, 0, UiControlAdjustment::Count, text};
        }

        UiActionSource Source() {
            const auto base = Base();
            return {base.owner, base.element};
        }

        void Select(UiTextEditBuffer &buffer, std::uint16_t anchor, std::uint16_t caret) {
            REQUIRE(buffer.Apply({UiTextEditKind::Select, {}, {anchor, caret}}).HasValue());
        }
    }  // namespace

    TEST_CASE("Editing uses complete extended graphemes including emoji and non-Latin marks", "[runtime_ui][text_edit]") {
        const std::array<std::string_view, 6> clusters{"e\xcc\x81",
                                                       "\xf0\x9f\x87\xb9\xf0\x9f\x87\xb7",
                                                       "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7",
                                                       "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd",
                                                       "\xd8\xa8\xd9\x90",
                                                       "\r\n"};
        for (const auto cluster : clusters) {
            auto buffer = Buffer(std::string("a") + std::string(cluster) + "z");
            REQUIRE(buffer.Snapshot().Value().graphemeCount == 3);
            REQUIRE(buffer.Apply({UiTextEditKind::Previous}).HasValue());
            REQUIRE(buffer.Apply({UiTextEditKind::Backspace}).HasValue());
            CHECK(buffer.Snapshot().Value().text.View() == "az");
            REQUIRE(buffer.Apply({UiTextEditKind::Undo}).HasValue());
            CHECK(buffer.Snapshot().Value().graphemeCount == 3);
            REQUIRE(buffer.Apply({UiTextEditKind::DeleteForward}).HasValue());
            CHECK(buffer.Snapshot().Value().text.View() == std::string("a") + std::string(cluster));
        }
    }

    TEST_CASE("Selection navigation and clipboard preserve complete ranges and owned copies", "[runtime_ui][text_edit]") {
        auto buffer = Buffer("abc");
        Select(buffer, 2, 1);
        auto copied = buffer.Apply({UiTextEditKind::Copy});
        REQUIRE(copied.HasValue());
        CHECK(copied.Value().clipboard->View() == "b");
        REQUIRE(buffer.Apply({UiTextEditKind::Previous}).HasValue());
        CHECK(buffer.Snapshot().Value().selection == UiTextSelection{1, 1});
        REQUIRE(buffer.Apply({UiTextEditKind::Next, {}, {}, true}).HasValue());
        CHECK(buffer.Snapshot().Value().selection == UiTextSelection{1, 2});
        auto cut = buffer.Apply({UiTextEditKind::Cut});
        REQUIRE(cut.HasValue());
        CHECK(cut.Value().textChanged);
        CHECK(cut.Value().clipboard->View() == "b");
        REQUIRE(buffer.Apply({UiTextEditKind::Paste, *cut.Value().clipboard}).HasValue());
        CHECK(buffer.Snapshot().Value().text.View() == "abc");
        CHECK(copied.Value().clipboard->View() == "b");
        REQUIRE(buffer.Apply({UiTextEditKind::Home}).HasValue());
        REQUIRE(buffer.Apply({UiTextEditKind::Previous}).HasValue());
        CHECK(buffer.Snapshot().Value().selection.caret == 0);
        REQUIRE(buffer.Apply({UiTextEditKind::End, {}, {}, true}).HasValue());
        CHECK(buffer.Snapshot().Value().selection == UiTextSelection{0, 3});
        REQUIRE(buffer.Apply({UiTextEditKind::Next}).HasValue());
        CHECK(buffer.Snapshot().Value().selection == UiTextSelection{3, 3});
    }

    TEST_CASE("Joining marks resegment the result and never leave a caret inside a grapheme", "[runtime_ui][text_edit]") {
        auto buffer = Buffer("ab");
        Select(buffer, 1, 1);
        REQUIRE(buffer.Apply({UiTextEditKind::Insert, Text("\xcc\x81")}).HasValue());
        CHECK(buffer.Snapshot().Value().graphemeCount == 2);
        CHECK(buffer.Snapshot().Value().selection.caret == 1);
        REQUIRE(buffer.Apply({UiTextEditKind::Backspace}).HasValue());
        CHECK(buffer.Snapshot().Value().text.View() == "b");
        auto joined = Buffer("\xf0\x9f\x91\xa8\xf0\x9f\x91\xa9");
        Select(joined, 1, 1);
        REQUIRE(joined.Apply({UiTextEditKind::Insert, Text("\xe2\x80\x8d")}).HasValue());
        CHECK(joined.Snapshot().Value().graphemeCount == 1);
        CHECK(joined.Snapshot().Value().selection.caret == 1);
    }

    TEST_CASE("Undo has a finite latest horizon and new edits discard redo without changing reads", "[runtime_ui][text_edit]") {
        UiTextEditPolicy policy;
        policy.undoDepth = 2;
        auto buffer = Buffer({}, policy);
        for (auto value : {"a", "b", "c"})
            REQUIRE(buffer.Apply({UiTextEditKind::Insert, Text(value)}).HasValue());
        REQUIRE(buffer.Apply({UiTextEditKind::Undo}).Value().textChanged);
        CHECK(buffer.Snapshot().Value().text.View() == "ab");
        REQUIRE(buffer.Apply({UiTextEditKind::Undo}).Value().textChanged);
        CHECK(buffer.Snapshot().Value().text.View() == "a");
        CHECK_FALSE(buffer.Apply({UiTextEditKind::Undo}).Value().textChanged);
        REQUIRE(buffer.Apply({UiTextEditKind::Redo}).HasValue());
        REQUIRE(buffer.Apply({UiTextEditKind::Copy}).HasValue());
        REQUIRE(buffer.Apply({UiTextEditKind::Redo}).HasValue());
        CHECK(buffer.Snapshot().Value().text.View() == "abc");
        REQUIRE(buffer.Apply({UiTextEditKind::Undo}).HasValue());
        REQUIRE(buffer.Apply({UiTextEditKind::Insert, Text("d")}).HasValue());
        CHECK_FALSE(buffer.Apply({UiTextEditKind::Redo}).Value().textChanged);
        CHECK(buffer.Snapshot().Value().text.View() == "abd");
        policy.undoDepth = 0;
        auto noHistory = Buffer({}, policy);
        REQUIRE(noHistory.Apply({UiTextEditKind::Insert, Text("x")}).HasValue());
        CHECK_FALSE(noHistory.Apply({UiTextEditKind::Undo}).Value().textChanged);
    }

    TEST_CASE("Rejected malformed and over-capacity edits preserve text selection and undo", "[runtime_ui][text_edit]") {
        UiTextEditPolicy policy{4, 2, 2};
        auto buffer = Buffer("a", policy);
        REQUIRE(buffer.Apply({UiTextEditKind::Insert, Text("b")}).HasValue());
        CHECK(buffer.Apply({UiTextEditKind::Insert, Text("c")}).HasError());
        CHECK(buffer.Apply({UiTextEditKind::Paste, Text("12345")}).HasError());
        CHECK(buffer.Apply({UiTextEditKind::Select, {}, {0, 3}}).HasError());
        UiActionText invalid;
        invalid.size = 1;
        invalid.bytes[0] = static_cast<char>(0xff);
        CHECK(buffer.Apply({UiTextEditKind::Paste, invalid}).HasError());
        CHECK(buffer.Apply({UiTextEditKind::Home, Text("x")}).HasError());
        CHECK(buffer.Apply({UiTextEditKind::Count}).HasError());
        CHECK(buffer.Apply({UiTextEditKind::Undo, {}, {}, true}).HasError());
        CHECK(buffer.Reset(invalid).HasError());
        CHECK(buffer.Snapshot().Value().text.View() == "ab");
        CHECK(buffer.Snapshot().Value().selection.caret == 2);
        REQUIRE(buffer.Apply({UiTextEditKind::Undo}).HasValue());
        CHECK(buffer.Snapshot().Value().text.View() == "a");
        invalid.size = 257;
        CHECK(UiTextEditBuffer::Create({}, invalid).HasError());
        policy.maximumBytes = 0;
        CHECK(UiTextEditBuffer::Create(policy, {}).HasError());
        policy = {};
        policy.validation = UiTextValidation::Count;
        CHECK(UiTextEditBuffer::Create(policy, {}).HasError());
        policy = {};
        policy.undoDepth = MaximumUiTextUndoDepth + 1;
        CHECK(UiTextEditBuffer::Create(policy, {}).HasError());
    }

    TEST_CASE("Validation is applied atomically to the complete draft and initial owner values", "[runtime_ui][text_edit]") {
        UiTextEditPolicy policy;
        policy.validation = UiTextValidation::SingleLine;
        for (std::string_view separator : {"\n", "\r", "\xc2\x85", "\xe2\x80\xa8", "\xe2\x80\xa9"}) {
            CHECK(UiTextEditBuffer::Create(policy, Text(separator)).HasError());
            auto buffer = Buffer("a", policy);
            CHECK(buffer.Apply({UiTextEditKind::Insert, Text(separator)}).HasError());
            CHECK(buffer.Snapshot().Value().text.View() == "a");
        }
        policy.validation = UiTextValidation::AsciiDigits;
        auto number = Buffer("12", policy);
        REQUIRE(number.Apply({UiTextEditKind::SelectAll}).HasValue());
        CHECK(number.Apply({UiTextEditKind::Paste, Text("x")}).HasError());
        CHECK(number.Snapshot().Value().selection == UiTextSelection{0, 2});
        REQUIRE(number.Apply({UiTextEditKind::Backspace}).HasValue());
        CHECK(number.Snapshot().Value().text.size == 0);
        REQUIRE(number.Apply({UiTextEditKind::Insert, Text("3")}).HasValue());
    }

    TEST_CASE("Password display masks graphemes and cannot copy or cut semantic bytes", "[runtime_ui][text_edit]") {
        UiTextEditPolicy policy;
        policy.password = true;
        auto secret = Buffer("e\xcc\x81x", policy);
        CHECK(secret.Display().Value().View() == "\xe2\x80\xa2\xe2\x80\xa2");
        REQUIRE(secret.Apply({UiTextEditKind::SelectAll}).HasValue());
        CHECK(secret.Apply({UiTextEditKind::Copy}).HasError());
        CHECK(secret.Apply({UiTextEditKind::Cut}).HasError());
        CHECK(secret.Snapshot().Value().text.View() == "e\xcc\x81x");
        auto full = Buffer(std::string(256, 'a'), policy);
        CHECK(full.Display().Value().size == 768);
        CHECK(full.Apply({UiTextEditKind::Insert, Text("a")}).HasError());
    }

    TEST_CASE("Editing and history success paths use fixed storage", "[runtime_ui][text_edit][allocation]") {
        auto buffer = Buffer("abc");
        const auto payload = Text("\xcc\x81");
        const auto before = ::Horo::Tests::AllocationProbe::Count();
        const auto inserted = buffer.Apply({UiTextEditKind::Insert, payload});
        const auto selected = buffer.Apply({UiTextEditKind::SelectAll});
        const auto copied = buffer.Apply({UiTextEditKind::Copy});
        const auto cut = buffer.Apply({UiTextEditKind::Cut});
        const auto undo = buffer.Apply({UiTextEditKind::Undo});
        const auto redo = buffer.Apply({UiTextEditKind::Redo});
        const auto display = buffer.Display();
        const auto after = ::Horo::Tests::AllocationProbe::Count();
        CHECK(after == before);
        REQUIRE(inserted.HasValue());
        REQUIRE(selected.HasValue());
        REQUIRE(copied.HasValue());
        REQUIRE(cut.HasValue());
        REQUIRE(undo.HasValue());
        REQUIRE(redo.HasValue());
        REQUIRE(display.HasValue());
    }

    TEST_CASE("Owner generation, focus, sequence and routed submit boundaries gate integrated edits", "[runtime_ui][text_edit][controls]") {
        auto control = Control("abc");
        CHECK(control.EditText(Source(), 1, {UiTextEditKind::Backspace}).HasError());
        REQUIRE(control.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
        REQUIRE(control.EditText(Source(), 2, {UiTextEditKind::Previous}).HasValue());
        REQUIRE(control.Handle(Input(UiControlInputKind::TextInput, 3, Text("x"))).HasValue());
        CHECK(control.TextEditSnapshot().Value().text.View() == "abxc");
        CHECK(control.EditText(Source(), 3, {UiTextEditKind::Undo}).HasError());
        auto foreign = Source();
        ++foreign.element.generation;
        CHECK(control.EditText(foreign, 4, {UiTextEditKind::Undo}).HasError());
        REQUIRE(control.EditText(Source(), 4, {UiTextEditKind::Undo}).HasValue());
        CHECK(control.TextEditSnapshot().Value().text.View() == "abc");
        REQUIRE(control.Handle(Input(UiControlInputKind::SubmitPress, 5)).HasValue());
        REQUIRE(control.Handle(Input(UiControlInputKind::SubmitRelease, 6)).HasValue());
        CHECK(control.EditText(Source(), 7, {UiTextEditKind::Backspace}).HasError());
        REQUIRE(control.SuppressDefault().HasValue());
        REQUIRE(control.EditText(Source(), 7, {UiTextEditKind::Backspace}).HasValue());
        REQUIRE(control.Handle(Input(UiControlInputKind::Cancel, 8)).HasValue());
        CHECK(control.TextEditSnapshot().Value().text.View() == "abc");
        REQUIRE(control.Handle(Input(UiControlInputKind::FocusLost, 9)).HasValue());
        CHECK(control.EditText(Source(), 10, {UiTextEditKind::Insert, Text("z")}).HasError());
        REQUIRE(control.SetAvailability(UiControlAvailability::Disabled).HasValue());
        CHECK(control.EditText(Source(), 11, {UiTextEditKind::Copy}).HasError());
    }

    TEST_CASE("Reload and provider reconciliation validate policy and clear the old edit horizon", "[runtime_ui][text_edit][reload]") {
        auto source = Control("base");
        REQUIRE(source.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
        REQUIRE(source.EditText(Source(), 2, {UiTextEditKind::Insert, Text("x")}).HasValue());
        auto replacement = Control("new");
        REQUIRE(replacement.ReconcileReload(source, true).Value());
        CHECK(replacement.TextEditSnapshot().Value().text.View() == "basex");
        CHECK_FALSE(replacement.EditText(Source(), 1, {UiTextEditKind::Undo}).Value().textChanged);
        REQUIRE(replacement.Handle(Input(UiControlInputKind::Cancel, 2)).HasValue());
        CHECK(replacement.TextEditSnapshot().Value().text.View() == "base");
        UiTextEditPolicy digits;
        digits.validation = UiTextValidation::AsciiDigits;
        auto number = Control("1", digits);
        CHECK_FALSE(number.ReconcileReload(source, true).Value());
        CHECK(number.ReconcileValue(Text("x")).HasError());
        CHECK(number.TextEditSnapshot().Value().text.View() == "1");
        REQUIRE(number.ReconcileValue(Text("12")).HasValue());
        CHECK(number.TextDisplay().Value().View() == "12");
        REQUIRE(number.BeginRetirement().HasValue());
        CHECK(number.TextDisplay().HasError());
        number.Shutdown();
        number.Shutdown();
        CHECK(number.TextEditSnapshot().HasError());
        CHECK(number.EditText(Source(), 1, {UiTextEditKind::Insert, Text("3")}).HasError());
    }

    TEST_CASE("Text editing rejects foreign interaction sources without consuming the next sequence",
              "[runtime_ui][text_edit][presentation]") {
        auto control = Control("abc");
        REQUIRE(control.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
        REQUIRE(control.EditText(Source(), 2, {UiTextEditKind::Home}).HasValue());
        auto owner = control.Owner();
        owner.interaction = UiInteractionRevision::Create(4).Value();
        CHECK(control.EditText({owner, control.Element()}, 3, {UiTextEditKind::Backspace}).HasError());
        REQUIRE(control.EditText(Source(), 3, {UiTextEditKind::Insert, Text("x")}).HasValue());
        CHECK(control.TextEditSnapshot().Value().text.View() == "xabc");
    }

    TEST_CASE("Same-value provider reset fences prepared history even when the projected state is unchanged",
              "[runtime_ui][text_edit][reload]") {
        auto control = Control("a");
        REQUIRE(control.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
        REQUIRE(control.EditText(Source(), 2, {UiTextEditKind::Insert, Text("b")}).HasValue());
        REQUIRE(control.EditText(Source(), 3, {UiTextEditKind::Undo}).Value().textChanged);
        REQUIRE(control.Handle(Input(UiControlInputKind::SubmitPress, 4)).HasValue());
        REQUIRE(control.Handle(Input(UiControlInputKind::SubmitRelease, 5)).HasValue());
        REQUIRE(control.ApplyDefault().HasValue());
        const auto before = control.Snapshot().Value();
        const auto stamp = control.CaptureReloadStamp();
        REQUIRE(stamp.HasValue());
        REQUIRE(control.MatchesReloadStamp(stamp.Value()));
        REQUIRE(control.ReconcileValue(Text("a")).HasValue());
        CHECK(control.Snapshot().Value() == before);
        CHECK_FALSE(control.MatchesReloadStamp(stamp.Value()));
        auto refocus = Input(UiControlInputKind::PointerPress, 6);
        refocus.activationSource = UiControlActivationSource::Pointer;
        REQUIRE(control.Handle(refocus).HasValue());
        CHECK_FALSE(control.EditText(Source(), 7, {UiTextEditKind::Redo}).Value().textChanged);
    }

    TEST_CASE("Buffer reset and shutdown close independent copies without native session state", "[runtime_ui][text_edit][lifecycle]") {
        auto buffer = Buffer("a");
        REQUIRE(buffer.Apply({UiTextEditKind::Insert, Text("b")}).HasValue());
        auto copied = buffer;
        REQUIRE(buffer.Reset(Text("new")).HasValue());
        CHECK_FALSE(buffer.Apply({UiTextEditKind::Undo}).Value().textChanged);
        buffer.Shutdown();
        buffer.Shutdown();
        CHECK(buffer.Snapshot().HasError());
        CHECK(buffer.Display().HasError());
        CHECK(buffer.Reset({}).HasError());
        CHECK(buffer.Apply({UiTextEditKind::Copy}).HasError());
        CHECK(copied.Snapshot().Value().text.View() == "ab");
    }
}  // namespace Horo::Runtime::Ui
