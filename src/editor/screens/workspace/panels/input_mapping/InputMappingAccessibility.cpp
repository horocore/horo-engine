#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Runtime/InputAccessibility.h"
#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief A selected binding changed while its explicit draft was open; no replacement binding is implicitly edited. */
        const ErrorCodeDescriptor StaleBindingDraft{ErrorDomainId{"horo.editor.input"},
                                                    ErrorCode{"input.accessibility.draft_stale"},
                                                    ErrorSeverity::Error,
                                                    "The selected input binding changed while accessibility options were being edited.",
                                                    "Reopen the binding's accessibility options and apply the new draft.",
                                                    false,
                                                    true};

        /** @brief Formats exact integer microseconds as decimal milliseconds; invalid oversized input yields an empty draft. */
        std::array<char, 12> FormatMilliseconds(const std::uint64_t microseconds) noexcept {
            std::array<char, 12> result{};
            const auto whole = std::to_chars(result.data(), result.data() + 7, microseconds / 1000);
            if (whole.ec != std::errc{})
                return {};
            auto *end = whole.ptr;
            const auto fraction = microseconds % 1000;
            if (fraction != 0) {
                *end++ = '.';
                *end++ = static_cast<char>('0' + fraction / 100);
                *end++ = static_cast<char>('0' + (fraction / 10) % 10);
                *end++ = static_cast<char>('0' + fraction % 10);
                while (end[-1] == '0')
                    --end;
            }
            *end = '\0';
            return result;
        }

        /** @brief Parses exact unsigned decimal milliseconds into bounded integer microseconds, without float rounding or coercion. */
        std::optional<std::uint64_t> ParseMilliseconds(const std::array<char, 12> &field) noexcept {
            const auto end = std::ranges::find(field, '\0');
            if (end == field.end() || end == field.begin())
                return std::nullopt;
            std::uint64_t whole{}, fraction{};
            unsigned fractionalDigits{};
            bool separator{}, wholeDigit{};
            for (auto character = field.begin(); character != end; ++character) {
                if (*character == '.' || *character == ',') {
                    if (separator || !wholeDigit)
                        return std::nullopt;
                    separator = true;
                } else if (*character >= '0' && *character <= '9') {
                    const auto digit = static_cast<unsigned>(*character - '0');
                    if (separator) {
                        if (++fractionalDigits > 3)
                            return std::nullopt;
                        fraction = fraction * 10 + digit;
                    } else {
                        wholeDigit = true;
                        whole = whole * 10 + digit;
                        if (whole > 60000)
                            return std::nullopt;
                    }
                } else {
                    return std::nullopt;
                }
            }
            if (separator && fractionalDigits == 0)
                return std::nullopt;
            if (separator)
                while (fractionalDigits++ < 3)
                    fraction *= 10;
            const auto microseconds = whole * 1000 + fraction;
            return microseconds <= 60000000 ? std::optional{microseconds} : std::nullopt;
        }

        /** @brief Borrows the current effective binding inventory from the existing profile authority. */
        const std::vector<Input::InputBinding> &EffectiveBindings(const Input::ActionDescriptor &action,
                                                                  const Input::InputBindingProfile &profile) noexcept {
            const auto overrideValue = std::ranges::find(profile.overrides, action.id, &Input::BindingOverride::action);
            return overrideValue == profile.overrides.end() ? action.defaultBindings : overrideValue->bindings;
        }
    }  // namespace

    /** @copydoc BindingAccessibilityEditorDraft::FromPolicy */
    BindingAccessibilityEditorDraft BindingAccessibilityEditorDraft::FromPolicy(const Input::BindingAccessibilityPolicy &policy) noexcept {
        return {policy.digital.toggle,
                policy.stickyModifiers,
                policy.digital.repeat,
                FormatMilliseconds(policy.digital.holdMicroseconds),
                FormatMilliseconds(policy.digital.repeatDelayMicroseconds),
                FormatMilliseconds(policy.digital.repeatIntervalMicroseconds)};
    }

    /** @copydoc BindingAccessibilityEditorDraft::Evaluate */
    Result<Input::BindingAccessibilityPolicy> BindingAccessibilityEditorDraft::Evaluate() const {
        const auto hold = ParseMilliseconds(holdMilliseconds);
        const auto delay = ParseMilliseconds(repeatDelayMilliseconds);
        const auto interval = ParseMilliseconds(repeatIntervalMilliseconds);
        if (!hold || !delay || !interval)
            return Result<Input::BindingAccessibilityPolicy>::Failure(MakeError(Input::InputAccessibilityErrors::InvalidPolicy));
        const Input::BindingAccessibilityPolicy policy{{toggle, *hold, repeat, *delay, *interval}, stickyModifiers};
        if (!policy.IsValid())
            return Result<Input::BindingAccessibilityPolicy>::Failure(MakeError(Input::InputAccessibilityErrors::InvalidPolicy));
        return Result<Input::BindingAccessibilityPolicy>::Success(policy);
    }

    /** @copydoc ApplyBindingAccessibilityDraft */
    Result<void> ApplyBindingAccessibilityDraft(Input::InputRouter &router, const Input::ActionId &action, const std::size_t bindingIndex,
                                                const std::uint64_t expectedRevision, const Input::InputBinding &expected,
                                                const BindingAccessibilityEditorDraft &draft) {
        if (expectedRevision == 0 || router.RoutingState({}).configurationRevision != expectedRevision)
            return Result<void>::Failure(MakeError(StaleBindingDraft));
        const auto actions = router.Actions();
        const auto descriptor = std::ranges::find(actions, action, &Input::ActionDescriptor::id);
        if (descriptor == actions.end() || descriptor->valueType != Input::ActionValueType::Digital)
            return Result<void>::Failure(MakeError(StaleBindingDraft));
        const auto &bindings = EffectiveBindings(*descriptor, router.Profile());
        if (bindingIndex >= bindings.size() || bindings[bindingIndex] != expected)
            return Result<void>::Failure(MakeError(StaleBindingDraft));
        auto policy = draft.Evaluate();
        if (policy.HasError())
            return Result<void>::Failure(policy.ErrorValue());
        auto candidate = router.Profile();
        auto overrideValue = std::ranges::find(candidate.overrides, action, &Input::BindingOverride::action);
        if (overrideValue == candidate.overrides.end()) {
            candidate.overrides.push_back({action, bindings});
            overrideValue = std::prev(candidate.overrides.end());
        }
        overrideValue->bindings[bindingIndex].accessibility = std::move(policy).Value();
        return router.SetProfile(std::move(candidate));
    }

    /** @copydoc DrawBindingAccessibilityDraft */
    BindingAccessibilityDraftCommand DrawBindingAccessibilityDraft(BindingAccessibilityEditorDraft &draft,
                                                                   const EditorGuiContext &context) {
        const auto text = [&](const std::string_view key) -> const std::string & {
            return InputMappingPanel::kTextScope.Text(context.localization, key);
        };
        ImGui::Separator();
        ImGui::TextWrapped("%s", text("accessibility.title").c_str());
        ImGui::TextWrapped("%s", text("accessibility.hint").c_str());
        const auto flag = [&](const char *id, const std::string_view label, bool &value) {
            Ui::SettingRow(text(label).c_str(), nullptr, context.theme.fonts, [&] {
                static_cast<void>(Ui::CheckboxControl("##Value", &value, context.theme.fonts, 0, Ui::CheckboxTextTone::Primary));
            }, id, Ui::SettingRowLayout::Stacked);
        };
        flag("AccessibilityToggle", "accessibility.toggle", draft.toggle);
        flag("AccessibilitySticky", "accessibility.sticky", draft.stickyModifiers);
        flag("AccessibilityRepeat", "accessibility.repeat", draft.repeat);
        const auto duration = [&](const char *id, const std::string_view label, std::array<char, 12> &field) {
            ImGui::TextWrapped("%s", text(label).c_str());
            static_cast<void>(Ui::InputTextControl(id, field.data(), field.size(), context.theme.fonts));
        };
        duration("##AccessibilityHold", "accessibility.hold_ms", draft.holdMilliseconds);
        duration("##AccessibilityDelay", "accessibility.delay_ms", draft.repeatDelayMilliseconds);
        duration("##AccessibilityInterval", "accessibility.interval_ms", draft.repeatIntervalMilliseconds);
        const bool valid = draft.Evaluate().HasValue();
        if (!valid)
            ImGui::TextWrapped("%s", text("accessibility.invalid").c_str());
        const auto &apply = text("accessibility.apply");
        if (Ui::Button({.label = apply.c_str(), .enabled = valid, .font = context.theme.fonts.sans})) {
            return BindingAccessibilityDraftCommand::Apply;
        }
        const auto &cancel = text("accessibility.cancel");
        if (Ui::Button({.label = cancel.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans}))
            return BindingAccessibilityDraftCommand::Cancel;
        return BindingAccessibilityDraftCommand::None;
    }

    /** @copydoc InputMappingPanel::DrawAccessibilityControls */
    void InputMappingPanel::DrawAccessibilityControls(const Input::ActionDescriptor &action, const EditorGuiContext &context) {
        if (!accessibilityAction_ || *accessibilityAction_ != action.id || action.valueType != Input::ActionValueType::Digital) {
            accessibilityAction_.reset();
            return;
        }
        const auto &bindings = EffectiveBindings(action, router_->Profile());
        if (router_->RoutingState({}).configurationRevision != accessibilityConfigurationRevision_ ||
            accessibilityBinding_ >= bindings.size() || bindings[accessibilityBinding_] != accessibilityBaseline_) {
            accessibilityAction_.reset();
            ImGui::TextWrapped("%s", LocalizedText(context.localization, "accessibility.stale").c_str());
            return;
        }
        ImGui::PushID(action.id.Value().c_str());
        ImGui::PushID(static_cast<int>(accessibilityBinding_));
        const auto command = DrawBindingAccessibilityDraft(accessibilityDraft_, context);
        ImGui::PopID();
        ImGui::PopID();
        if (command == BindingAccessibilityDraftCommand::Cancel) {
            accessibilityAction_.reset();
        } else if (command == BindingAccessibilityDraftCommand::Apply) {
            const auto result =
                ApplyBindingAccessibilityDraft(*router_, action.id, accessibilityBinding_, accessibilityConfigurationRevision_,
                                               accessibilityBaseline_, accessibilityDraft_);
            if (result.HasError()) {
                statusMessage_ = result.ErrorValue().message;
            } else {
                accessibilityAction_.reset();
                statusMessage_ = LocalizedText(context.localization, "accessibility.applied");
            }
        }
    }
}  // namespace Horo::Editor
