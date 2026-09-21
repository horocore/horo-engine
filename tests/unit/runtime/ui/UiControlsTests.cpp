#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id AuthoredId(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 73) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiActionOwnerContext Context(const std::uint64_t ownerValue = 73) {
            const UiOwnershipGeneration owner = Owner(ownerValue);
            return {{owner, 1, 1},
                    {owner, 2, 1},
                    AuthoredId<UiDocumentId>(1),
                    RevisionValue<UiDocumentRevision>(1),
                    RevisionValue<UiRuntimeTreeRevision>(2),
                    RevisionValue<UiInteractionRevision>(3)};
        }

        UiActionSource Source(const UiActionOwnerContext &context = Context()) {
            return {context, {context.instance.ownership, 3, 1}};
        }

        UiControlDescriptorBase Base(const UiActionOwnerContext &context = Context(), const bool enabled = true,
                                     const UiControlRepeatPolicy repeat = {}) {
            return {context, Source(context).element, AuthoredId<UiActionId>(9), {}, enabled, true, repeat};
        }

        UiControlStateMachine MakeButton(const bool enabled = true, const UiControlRepeatPolicy repeat = {}) {
            auto machine = UiControlStateMachine::Create(UiButtonControlDescriptor{Base(Context(), enabled, repeat)});
            REQUIRE(machine.HasValue());
            return std::move(machine).Value();
        }

        UiControlInput Input(const UiControlInputKind kind, const std::uint64_t sequence,
                             const UiControlActivationSource source = UiControlActivationSource::Programmatic, const std::uint64_t tick = 0,
                             const UiControlAdjustment adjustment = UiControlAdjustment::Count, const UiActionText text = {}) {
            return {Source(), kind, source, sequence, tick, adjustment, text};
        }

        UiActionText Text(const std::string_view value) {
            const auto result = UiActionText::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Typed control descriptors validate kind-specific state and bounded payloads", "[runtime_ui][controls][validation]") {
            const UiControlDescriptor button = UiButtonControlDescriptor{Base()};
            REQUIRE(ValidateUiControlDescriptor(button).HasValue());
            CHECK(UiControlKindOf(button) == UiControlKind::Button);

            UiControlDescriptor invalidSlider = UiSliderControlDescriptor{Base(), 0.0, 1.0, 0.0, 0.5};
            ExpectError(ValidateUiControlDescriptor(invalidSlider), UiErrors::ControlDescriptorInvalid);

            UiControlDescriptor invalidText = UiTextInputControlDescriptor{Base(Context(), true, {true, 1, 1}), Text("a"), 8, true};
            ExpectError(ValidateUiControlDescriptor(invalidText), UiErrors::ControlDescriptorInvalid);

            UiActionPayload fullPayload;
            for (std::size_t index = 0; index < MaximumUiActionArguments; ++index)
                REQUIRE(fullPayload.Add(static_cast<std::uint64_t>(index)).HasValue());
            UiControlDescriptor invalidToggle = UiToggleControlDescriptor{UiControlDescriptorBase{Context(),
                                                                                                  Source().element,
                                                                                                  AuthoredId<UiActionId>(9),
                                                                                                  fullPayload,
                                                                                                  true,
                                                                                                  true,
                                                                                                  {}},
                                                                          false};
            ExpectError(ValidateUiControlDescriptor(invalidToggle), UiErrors::ControlDescriptorInvalid);

            UiControlDescriptor invalidValue = UiSliderControlDescriptor{Base(), 0.0, 1.0, std::numeric_limits<double>::infinity(), 0.0};
            ExpectError(ValidateUiControlDescriptor(invalidValue), UiErrors::ControlDescriptorInvalid);
        }

        TEST_CASE("Pointer and keyboard activation stage deterministic button defaults and honor disabled state",
                  "[runtime_ui][controls][activation]") {
            auto button = MakeButton();
            REQUIRE(button.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
            auto pressed = button.Handle(Input(UiControlInputKind::SubmitPress, 2, UiControlActivationSource::Keyboard));
            REQUIRE(pressed.HasValue());
            REQUIRE(std::get<UiButtonControlState>(pressed.Value().state).pressed);

            auto released = button.Handle(Input(UiControlInputKind::SubmitRelease, 3, UiControlActivationSource::Keyboard));
            REQUIRE(released.HasValue());
            CHECK(released.Value().defaultActionPending);
            const auto beforeDefault = button.Snapshot();
            REQUIRE(beforeDefault.HasValue());
            CHECK_FALSE(std::get<UiButtonControlState>(beforeDefault.Value()).pressed);

            auto keyboardAction = button.ApplyDefault();
            REQUIRE(keyboardAction.HasValue());
            REQUIRE(keyboardAction.Value().has_value());
            CHECK(keyboardAction.Value()->kind == UiControlActionKind::Activate);
            CHECK(keyboardAction.Value()->activationSource == UiControlActivationSource::Keyboard);
            CHECK(keyboardAction.Value()->eventSequence == 3);
            CHECK_FALSE(button.ApplyDefault().Value().has_value());

            REQUIRE(button.Handle(Input(UiControlInputKind::PointerPress, 4, UiControlActivationSource::Pointer)).HasValue());
            REQUIRE(button.Handle(Input(UiControlInputKind::PointerRelease, 5, UiControlActivationSource::Pointer)).HasValue());
            const auto pointerAction = button.ApplyDefault();
            REQUIRE(pointerAction.HasValue());
            REQUIRE(pointerAction.Value().has_value());
            CHECK(pointerAction.Value()->activationSource == UiControlActivationSource::Pointer);

            REQUIRE(button.SetAvailability(UiControlAvailability::Disabled).HasValue());
            const auto disabled = button.Handle(Input(UiControlInputKind::PointerPress, 6, UiControlActivationSource::Pointer));
            REQUIRE(disabled.HasValue());
            CHECK(disabled.Value().transition == UiControlTransitionKind::IgnoredDisabled);
            CHECK_FALSE(std::get<UiButtonControlState>(disabled.Value().state).pressed);
        }

        TEST_CASE("Toggle values change only when the routed default is applied and cancel clears press state",
                  "[runtime_ui][controls][default_action]") {
            UiControlStateMachine toggle = std::move(UiControlStateMachine::Create(UiToggleControlDescriptor{Base(), false})).Value();
            REQUIRE(toggle.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
            REQUIRE(toggle.Handle(Input(UiControlInputKind::SubmitPress, 2, UiControlActivationSource::Keyboard)).HasValue());
            REQUIRE(toggle.Handle(Input(UiControlInputKind::SubmitRelease, 3, UiControlActivationSource::Keyboard)).HasValue());

            CHECK_FALSE(std::get<UiToggleControlState>(toggle.Snapshot().Value()).checked);
            REQUIRE(toggle.SuppressDefault().HasValue());
            CHECK_FALSE(std::get<UiToggleControlState>(toggle.Snapshot().Value()).checked);

            REQUIRE(toggle.Handle(Input(UiControlInputKind::SubmitPress, 4, UiControlActivationSource::Keyboard)).HasValue());
            REQUIRE(toggle.Handle(Input(UiControlInputKind::Cancel, 5)).HasValue());
            CHECK_FALSE(std::get<UiToggleControlState>(toggle.Snapshot().Value()).pressed);
            CHECK_FALSE(toggle.ApplyDefault().Value().has_value());

            REQUIRE(toggle.Handle(Input(UiControlInputKind::SubmitPress, 6, UiControlActivationSource::Keyboard)).HasValue());
            REQUIRE(toggle.Handle(Input(UiControlInputKind::SubmitRelease, 7, UiControlActivationSource::Keyboard)).HasValue());
            const auto applied = toggle.ApplyDefault();
            REQUIRE(applied.HasValue());
            REQUIRE(applied.Value().has_value());
            CHECK(applied.Value()->kind == UiControlActionKind::Toggle);
            REQUIRE(applied.Value()->payload.Size() == 1);
            CHECK(std::get<bool>(applied.Value()->payload.Values().front()));
            CHECK(std::get<UiToggleControlState>(toggle.Snapshot().Value()).checked);
        }

        TEST_CASE("Held activation repeats from owner ticks without hot-path allocation", "[runtime_ui][controls][repeat]") {
            auto button = MakeButton(true, {true, 2, 2});
            REQUIRE(button.Handle(Input(UiControlInputKind::FocusGained, 1, UiControlActivationSource::Programmatic, 1)).HasValue());
            REQUIRE(button.Handle(Input(UiControlInputKind::SubmitPress, 2, UiControlActivationSource::Keyboard, 2)).HasValue());
            ExpectError(button.Handle(Input(UiControlInputKind::RepeatTick, 3, UiControlActivationSource::Pointer, 3)),
                        UiErrors::ControlInputInvalid);
            const auto early = button.Handle(Input(UiControlInputKind::RepeatTick, 3, UiControlActivationSource::Keyboard, 3));
            REQUIRE(early.HasValue());
            CHECK_FALSE(early.Value().defaultActionPending);

            const auto allocationsBefore = ::Horo::Tests::AllocationProbe::Count();
            const auto repeated = button.Handle(Input(UiControlInputKind::RepeatTick, 4, UiControlActivationSource::Keyboard, 4));
            REQUIRE(repeated.HasValue());
            const auto action = button.ApplyDefault();
            const auto allocationsAfter = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(action.HasValue());
            REQUIRE(action.Value().has_value());
            CHECK(action.Value()->repeated);
            CHECK(action.Value()->eventSequence == 4);
            CHECK(allocationsAfter == allocationsBefore);

            const auto nextEarly = button.Handle(Input(UiControlInputKind::RepeatTick, 5, UiControlActivationSource::Keyboard, 5));
            REQUIRE(nextEarly.HasValue());
            CHECK_FALSE(nextEarly.Value().defaultActionPending);
            REQUIRE(button.Handle(Input(UiControlInputKind::RepeatTick, 6, UiControlActivationSource::Keyboard, 6)).HasValue());
            REQUIRE(button.ApplyDefault().Value().has_value());
            REQUIRE(button.Handle(Input(UiControlInputKind::SubmitRelease, 7, UiControlActivationSource::Keyboard, 7)).HasValue());
            REQUIRE(button.ApplyDefault().Value().has_value());
        }

        TEST_CASE("Text input owns editing state, bounded UTF-8 text and cancel restoration", "[runtime_ui][controls][text]") {
            UiTextInputControlDescriptor descriptor{Base(), Text("old"), 8, true};
            auto machine = UiControlStateMachine::Create(descriptor);
            REQUIRE(machine.HasValue());
            auto textInput = std::move(machine).Value();

            REQUIRE(textInput.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
            REQUIRE(textInput
                        .Handle(Input(UiControlInputKind::TextInput, 2, UiControlActivationSource::Keyboard, 0, UiControlAdjustment::Count,
                                      Text("new")))
                        .HasValue());
            CHECK(std::get<UiTextInputControlState>(textInput.Snapshot().Value()).text.View() == "oldnew");

            REQUIRE(textInput.Handle(Input(UiControlInputKind::Cancel, 3)).HasValue());
            const auto cancelled = std::get<UiTextInputControlState>(textInput.Snapshot().Value());
            CHECK(cancelled.text.View() == "old");
            CHECK_FALSE(cancelled.editing);
            CHECK(cancelled.focused);

            REQUIRE(textInput.Handle(Input(UiControlInputKind::SubmitPress, 4, UiControlActivationSource::Keyboard)).HasValue());
            REQUIRE(textInput
                        .Handle(Input(UiControlInputKind::TextInput, 5, UiControlActivationSource::Keyboard, 0, UiControlAdjustment::Count,
                                      Text("x")))
                        .HasValue());
            REQUIRE(textInput.Handle(Input(UiControlInputKind::SubmitRelease, 6, UiControlActivationSource::Keyboard)).HasValue());
            const auto submitted = textInput.ApplyDefault();
            REQUIRE(submitted.HasValue());
            REQUIRE(submitted.Value().has_value());
            CHECK(submitted.Value()->kind == UiControlActionKind::Submit);
            CHECK(std::get<UiActionText>(submitted.Value()->payload.Values().back()).View() == "oldx");
            CHECK_FALSE(std::get<UiTextInputControlState>(textInput.Snapshot().Value()).editing);

            REQUIRE(textInput.Handle(Input(UiControlInputKind::SubmitPress, 7, UiControlActivationSource::Keyboard)).HasValue());
            const auto overflow = textInput.Handle(Input(UiControlInputKind::TextInput, 8, UiControlActivationSource::Keyboard, 0,
                                                         UiControlAdjustment::Count, Text("012345678")));
            ExpectError(overflow, UiErrors::ControlCapacityExceeded);
            REQUIRE(textInput.Handle(Input(UiControlInputKind::Cancel, 9)).HasValue());
        }

        TEST_CASE("Slider adjustments are typed default actions and focus/lifetime transitions are fail-closed",
                  "[runtime_ui][controls][lifetime]") {
            UiSliderControlDescriptor descriptor{Base(), 0.0, 1.0, 0.25, 0.5};
            auto machine = UiControlStateMachine::Create(descriptor);
            REQUIRE(machine.HasValue());
            auto slider = std::move(machine).Value();
            REQUIRE(slider.Handle(Input(UiControlInputKind::FocusGained, 1)).HasValue());
            const auto pointerPressed = slider.Handle(Input(UiControlInputKind::PointerPress, 2, UiControlActivationSource::Pointer, 2));
            REQUIRE(pointerPressed.HasValue());
            CHECK(pointerPressed.Value().transition == UiControlTransitionKind::Pressed);
            CHECK(std::get<UiSliderControlState>(slider.Snapshot().Value()).pressed);
            REQUIRE(slider.Handle(Input(UiControlInputKind::PointerRelease, 3, UiControlActivationSource::Pointer, 3)).HasValue());
            CHECK_FALSE(std::get<UiSliderControlState>(slider.Snapshot().Value()).pressed);
            REQUIRE(slider
                        .Handle(Input(UiControlInputKind::AdjustPress, 4, UiControlActivationSource::Keyboard, 4,
                                      UiControlAdjustment::Increase))
                        .HasValue());
            CHECK(std::get<UiSliderControlState>(slider.Snapshot().Value()).value == 0.5);
            const auto adjusted = slider.ApplyDefault();
            REQUIRE(adjusted.HasValue());
            REQUIRE(adjusted.Value().has_value());
            CHECK(adjusted.Value()->kind == UiControlActionKind::ValueChanged);
            CHECK(std::get<double>(adjusted.Value()->payload.Values().back()) == 0.75);
            CHECK(std::get<UiSliderControlState>(slider.Snapshot().Value()).value == 0.75);

            REQUIRE(slider
                        .Handle(Input(UiControlInputKind::AdjustRelease, 5, UiControlActivationSource::Keyboard, 5,
                                      UiControlAdjustment::Increase))
                        .HasValue());
            REQUIRE(slider.SetAvailability(UiControlAvailability::Disabled).HasValue());
            const auto disabled = slider.Handle(
                Input(UiControlInputKind::AdjustPress, 6, UiControlActivationSource::Keyboard, 6, UiControlAdjustment::Increase));
            REQUIRE(disabled.HasValue());
            CHECK(disabled.Value().transition == UiControlTransitionKind::IgnoredDisabled);

            REQUIRE(slider.BeginRetirement().HasValue());
            ExpectError(slider.Handle(Input(UiControlInputKind::FocusGained, 7)), UiErrors::ControlLifecycleUnavailable);
            REQUIRE(slider.Snapshot().HasValue());
            slider.Shutdown();
            slider.Shutdown();
            CHECK(slider.LifecycleState() == UiControlLifecycleState::Stopped);
            ExpectError(slider.Snapshot(), UiErrors::ControlLifecycleUnavailable);
        }

        TEST_CASE("Controls reject foreign and out-of-order evidence before mutating state", "[runtime_ui][controls][fencing]") {
            auto button = MakeButton();
            const auto foreign = Input(UiControlInputKind::FocusGained, 1);
            auto foreignInput = foreign;
            foreignInput.source = Source(Context(74));
            ExpectError(button.Handle(foreignInput), UiErrors::ControlSourceStale);

            REQUIRE(button.Handle(Input(UiControlInputKind::FocusGained, 2)).HasValue());
            ExpectError(button.Handle(Input(UiControlInputKind::SubmitPress, 2, UiControlActivationSource::Keyboard)),
                        UiErrors::ControlSequenceInvalid);

            REQUIRE(button.Handle(Input(UiControlInputKind::SubmitPress, 3, UiControlActivationSource::Keyboard)).HasValue());
            REQUIRE(button.Handle(Input(UiControlInputKind::SubmitRelease, 4, UiControlActivationSource::Keyboard)).HasValue());
            ExpectError(button.Handle(Input(UiControlInputKind::FocusGained, 5)), UiErrors::ControlDefaultPending);
            REQUIRE(button.Handle(Input(UiControlInputKind::FocusLost, 6)).HasValue());
            CHECK_FALSE(button.ApplyDefault().Value().has_value());
        }

        TEST_CASE("Control errors use unique actionable descriptors", "[runtime_ui][controls][errors]") {
            const std::array descriptors{&UiErrors::ControlDescriptorInvalid, &UiErrors::ControlInputInvalid,
                                         &UiErrors::ControlSourceStale,       &UiErrors::ControlDefaultPending,
                                         &UiErrors::ControlDefaultInvalid,    &UiErrors::ControlCapacityExceeded,
                                         &UiErrors::ControlSequenceInvalid,   &UiErrors::ControlLifecycleUnavailable};
            std::set<std::string_view> codes;
            for (const ErrorCodeDescriptor *descriptor : descriptors) {
                REQUIRE(descriptor->domain.Value() == "horo.runtime_ui");
                REQUIRE(codes.insert(descriptor->code.Value()).second);
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
            }
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
