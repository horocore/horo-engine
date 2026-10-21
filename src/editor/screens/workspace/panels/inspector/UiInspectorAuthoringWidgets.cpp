#include "editor/screens/workspace/panels/inspector/UiInspectorAuthoringWidgets.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <new>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Resolve editor labels only; edited game messages remain separate canonical references. */
        const char *Label(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key).c_str();
        }

        /** @brief Capture bounded canonical source text outside unchanged draw frames. */
        template <std::size_t N> void CaptureText(std::array<char, N> &buffer, const std::string_view text) {
            buffer.fill('\0');
            std::copy_n(text.begin(), std::min(text.size(), N - 1), buffer.begin());
        }

        /** @brief Read only within the retained buffer even for malformed test drafts. */
        template <std::size_t N> std::string_view Contents(const std::array<char, N> &buffer) {
            const auto end = std::find(buffer.begin(), buffer.end(), '\0');
            return {buffer.data(), static_cast<std::size_t>(std::distance(buffer.begin(), end))};
        }

        /** @brief All text inputs use the shared control and preserve failed input for correction. */
        template <std::size_t N>
        bool Text(const EditorGuiContext &context, const char *key, std::array<char, N> &buffer, const bool valid = true) {
            Ui::FieldLabel(Label(context, key), context.theme.fonts);
            return Ui::InputTextControl(key, buffer.data(), buffer.size(), context.theme.fonts, {.error = !valid});
        }

        /** @brief Exact integral editing never narrows identities through floating point. */
        template <typename T> bool Integer(const EditorGuiContext &context, const char *key, T &value, UiInspectorInteger &text) {
            if (!Text(context, key, text.bytes, text.valid))
                return false;
            static_cast<void>(text.Parse(value));
            return true;
        }

        /** @brief Exact finite double editing preserves precision and invalid raw text. */
        bool Number(const EditorGuiContext &context, const char *key, double &value, UiInspectorDouble &text) {
            if (!Text(context, key, text.bytes, text.valid))
                return false;
            static_cast<void>(text.Parse(value));
            return true;
        }

        /** @brief Explicit user insertion supplies no invented argument name or enum identity. */
        UiLocalizedArgumentValue ArgumentValue(const int index) {
            switch (index) {
                case 0:
                    return std::int64_t{};
                case 1:
                    return double{};
                case 2:
                    return false;
                case 3:
                    return UiLocalizedDateTime{};
                case 4:
                    return UiLocalizedDuration{};
                case 5:
                    return UiLocalizedStableEnum{};
                case 6:
                    return std::string{};
                default:
                    return UiLocalizedShortcut{};
            }
        }

        /** @brief Shared localized action with a bounded enabled contract. */
        bool Action(const EditorGuiContext &context, const char *key, const bool enabled = true) {
            return Ui::Button({.label = Label(context, key), .enabled = enabled, .font = context.theme.fonts.sans});
        }

        /** @brief Canonical enum order is checked against the runtime public vocabulary at compile time. */
        bool Role(const EditorGuiContext &context, UiAccessibilityRole &role) {
            constexpr std::array keys{"workspace.ui_inspector.role.application", "workspace.ui_inspector.role.window",
                                      "workspace.ui_inspector.role.screen",      "workspace.ui_inspector.role.dialog",
                                      "workspace.ui_inspector.role.alert",       "workspace.ui_inspector.role.group",
                                      "workspace.ui_inspector.role.heading",     "workspace.ui_inspector.role.static_text",
                                      "workspace.ui_inspector.role.button",      "workspace.ui_inspector.role.toggle",
                                      "workspace.ui_inspector.role.checkbox",    "workspace.ui_inspector.role.radio",
                                      "workspace.ui_inspector.role.slider",      "workspace.ui_inspector.role.text_field",
                                      "workspace.ui_inspector.role.link",        "workspace.ui_inspector.role.image",
                                      "workspace.ui_inspector.role.progress",    "workspace.ui_inspector.role.list",
                                      "workspace.ui_inspector.role.list_item",   "workspace.ui_inspector.role.menu",
                                      "workspace.ui_inspector.role.menu_item",   "workspace.ui_inspector.role.tab",
                                      "workspace.ui_inspector.role.tab_item",    "workspace.ui_inspector.role.tree",
                                      "workspace.ui_inspector.role.tree_item",   "workspace.ui_inspector.role.table",
                                      "workspace.ui_inspector.role.row",         "workspace.ui_inspector.role.cell",
                                      "workspace.ui_inspector.role.scroll_view"};
            static_assert(keys.size() == static_cast<std::size_t>(UiAccessibilityRole::ScrollView) + 1);
            std::array<const char *, keys.size()> labels{};
            for (std::size_t i = 0; i < keys.size(); ++i) {
                // Labels are cached by the localization authority, not interpreted as authored data.
                labels[i] = Label(context, keys[i]);
            }
            int selected = static_cast<int>(role);
            if (!Ui::DrawComboPropRow(Label(context, "workspace.ui_inspector.accessibility_role"), "accessibility_role", selected, labels,
                                      context.theme.fonts))
                return false;
            role = static_cast<UiAccessibilityRole>(selected);
            return true;
        }
    }  // namespace

    /** @copydoc UiInspectorMessageDraft::Capture */
    void UiInspectorMessageDraft::Capture(const std::optional<UiLocalizedText> &source) {
        enabled_ = source.has_value();
        valid_ = !source || source->IsValid();
        CaptureText(namespace_, source ? source->Key().namespaceId : "");
        CaptureText(key_, source ? source->Key().localKey : "");
        CaptureText(fallback_, source ? source->FallbackText() : "");
        policy_ = source ? source->FailurePolicy() : UiLocalizedTextFailurePolicy::UseFallback;
        arguments_.clear();
        if (source)
            arguments_.assign(source->Arguments().begin(), source->Arguments().end());
        SelectArgument(arguments_.empty() ? -1 : 0);
    }

    /** @copydoc UiInspectorMessageDraft::SelectArgument */
    void UiInspectorMessageDraft::SelectArgument(const int index) {
        selected_ = index;
        argumentName_ = {};
        argumentText_ = {};
        integers_ = {};
        number_ = {};
        if (index < 0)
            return;
        const auto &argument = arguments_[static_cast<std::size_t>(index)];
        CaptureText(argumentName_, argument.name);
        std::visit([&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::int64_t>)
                integers_[0].Capture(value);
            else if constexpr (std::is_same_v<T, double>)
                number_.Capture(value);
            else if constexpr (std::is_same_v<T, UiLocalizedDateTime>)
                integers_[0].Capture(value.unixMilliseconds);
            else if constexpr (std::is_same_v<T, UiLocalizedDuration>)
                integers_[0].Capture(value.milliseconds);
            else if constexpr (std::is_same_v<T, UiLocalizedStableEnum>) {
                integers_[0].Capture(value.typeId);
                integers_[1].Capture(value.value);
            } else if constexpr (std::is_same_v<T, UiLocalizedShortcut>) {
                integers_[0].Capture(value.modifiers);
                integers_[1].Capture(value.key);
            } else if constexpr (std::is_same_v<T, std::string>)
                CaptureText(argumentText_, value);
        }, argument.value);
    }

    /** @copydoc UiInspectorMessageDraft::ArgumentTextValid */
    bool UiInspectorMessageDraft::ArgumentTextValid() const noexcept {
        return integers_[0].valid && integers_[1].valid && number_.valid;
    }

    /** @copydoc UiInspectorMessageDraft::DrawArgumentValue */
    bool UiInspectorMessageDraft::DrawArgumentValue(const EditorGuiContext &context, UiLocalizedArgument &argument) {
        return std::visit([&](auto &value) -> bool {
            using T = std::decay_t<decltype(value)>;
            constexpr const char *key = "workspace.ui_inspector.argument_value";
            if constexpr (std::is_same_v<T, std::int64_t>)
                return Integer(context, key, value, integers_[0]);
            else if constexpr (std::is_same_v<T, double>)
                return Number(context, key, value, number_);
            else if constexpr (std::is_same_v<T, bool>)
                return Ui::CheckboxControl(Label(context, key), &value, context.theme.fonts);
            else if constexpr (std::is_same_v<T, UiLocalizedDateTime>)
                return Integer(context, key, value.unixMilliseconds, integers_[0]);
            else if constexpr (std::is_same_v<T, UiLocalizedDuration>)
                return Integer(context, key, value.milliseconds, integers_[0]);
            else if constexpr (std::is_same_v<T, std::string>) {
                if (!Text(context, key, argumentText_))
                    return false;
                value = Contents(argumentText_);
                return true;
            } else if constexpr (std::is_same_v<T, UiLocalizedStableEnum>) {
                bool changed = Integer(context, "workspace.ui_inspector.argument_enum_type", value.typeId, integers_[0]);
                return Integer(context, key, value.value, integers_[1]) || changed;
            } else {
                bool changed = Integer(context, "workspace.ui_inspector.argument_modifiers", value.modifiers, integers_[0]);
                return Integer(context, key, value.key, integers_[1]) || changed;
            }
        }, argument.value);
    }

    /** @copydoc UiInspectorMessageDraft::DrawArguments */
    bool UiInspectorMessageDraft::DrawArguments(const EditorGuiContext &context) {
        bool changed{};
        if (!arguments_.empty()) {
            ImGui::BeginDisabled(!ArgumentTextValid());
            int selected = selected_;
            if (Ui::ComboControl("arguments", &selected, static_cast<int>(arguments_.size()),
                                 Ui::ComboItemSource{.label =
                                                         [this](int i) {
                return arguments_[static_cast<std::size_t>(i)].name.c_str();
            }},
                                 context.theme.fonts))
                SelectArgument(selected);
            ImGui::EndDisabled();
            auto &argument = arguments_[static_cast<std::size_t>(selected_)];
            if (Text(context, "workspace.ui_inspector.argument_name", argumentName_)) {
                argument.name = Contents(argumentName_);
                changed = true;
            }
            const char *types[]{Label(context, "workspace.ui_inspector.arg.integer"),
                                Label(context, "workspace.ui_inspector.arg.decimal"),
                                Label(context, "workspace.ui_inspector.arg.boolean"),
                                Label(context, "workspace.ui_inspector.arg.date_time"),
                                Label(context, "workspace.ui_inspector.arg.duration"),
                                Label(context, "workspace.ui_inspector.arg.enum"),
                                Label(context, "workspace.ui_inspector.arg.text"),
                                Label(context, "workspace.ui_inspector.arg.shortcut")};
            int kind = static_cast<int>(argument.value.index());
            if (Ui::DrawComboPropRow(Label(context, "workspace.ui_inspector.argument_type"), "argument_type", kind, types,
                                     context.theme.fonts)) {
                argument.value = ArgumentValue(kind);
                SelectArgument(selected_);
                changed = true;
            }
            changed = DrawArgumentValue(context, argument) || changed;
            if (Action(context, "workspace.ui_inspector.remove_argument")) {
                arguments_.erase(arguments_.begin() + selected_);
                SelectArgument(arguments_.empty() ? -1 : 0);
                changed = true;
            }
        }
        if (Action(context, "workspace.ui_inspector.add_argument",
                   arguments_.size() < MaximumUiLocalizedArguments && ArgumentTextValid())) {
            arguments_.push_back({});
            SelectArgument(static_cast<int>(arguments_.size()) - 1);
            changed = true;
        }
        return changed;
    }

    /** @copydoc UiInspectorMessageDraft::Build */
    bool UiInspectorMessageDraft::Build(std::optional<UiLocalizedText> &destination) {
        if (!enabled_) {
            destination.reset();
            valid_ = true;
            return true;
        }
        valid_ = false;
        if (!ArgumentTextValid())
            return false;
        auto key = UiMessageKey::Create(Contents(namespace_), Contents(key_));
        if (key.HasError())
            return false;
        try {
            auto message = UiLocalizedText::Create(std::move(key).Value(), arguments_, std::string{Contents(fallback_)}, policy_);
            valid_ = message.HasValue();
            if (valid_)
                destination = std::move(message).Value();
        } catch (const std::bad_alloc &) {
            valid_ = false;
        }
        return valid_;
    }

    /** @copydoc UiInspectorMessageDraft::Draw */
    bool UiInspectorMessageDraft::Draw(const EditorGuiContext &context, const char *key, std::optional<UiLocalizedText> &destination) {
        ImGui::PushID(key);
        bool changed = Ui::CheckboxControl(Label(context, key), &enabled_, context.theme.fonts);
        if (enabled_) {
            changed = Text(context, "workspace.ui_inspector.message_namespace", namespace_, valid_) || changed;
            changed = Text(context, "workspace.ui_inspector.message_key", key_, valid_) || changed;
            changed = Text(context, "workspace.ui_inspector.message_fallback", fallback_, valid_) || changed;
            const char *policies[]{Label(context, "workspace.ui_inspector.failure.fallback"),
                                   Label(context, "workspace.ui_inspector.failure.placeholder"),
                                   Label(context, "workspace.ui_inspector.failure.required")};
            int policy = static_cast<int>(policy_);
            if (Ui::DrawComboPropRow(Label(context, "workspace.ui_inspector.failure_policy"), "failure", policy, policies,
                                     context.theme.fonts)) {
                policy_ = static_cast<UiLocalizedTextFailurePolicy>(policy);
                changed = true;
            }
            changed = DrawArguments(context) || changed;
        }
        if (changed)
            static_cast<void>(Build(destination));
        ImGui::PopID();
        return changed;
    }

    /** @copydoc UiInspectorAuthoringWidgets::CaptureInitial */
    void UiInspectorAuthoringWidgets::CaptureInitial(const UiElementAuthoredState &state) {
        initialKind_ = state.initial.index();
        scalar_ = {};
        initialText_ = {};
        initialTextValid_ = true;
        if (const auto *values = std::get_if<UiControlScalarValues>(&state.initial)) {
            scalar_[0].Capture(values->minimum);
            scalar_[1].Capture(values->maximum);
            scalar_[2].Capture(values->step);
            scalar_[3].Capture(values->value);
        }
        if (const auto *text = std::get_if<std::string>(&state.initial))
            CaptureText(initialText_, *text);
    }

    /** @copydoc UiInspectorAuthoringWidgets::Capture */
    void UiInspectorAuthoringWidgets::Capture(const UiElementPresentation &source) {
        CaptureInitial(source.state);
        content_.Capture(source.localizedText);
        name_.Capture(source.accessibility ? source.accessibility->name : std::nullopt);
        description_.Capture(source.accessibility ? source.accessibility->description : std::nullopt);
    }

    /** @copydoc UiInspectorAuthoringWidgets::DrawState */
    bool UiInspectorAuthoringWidgets::DrawState(const EditorGuiContext &context, const UiElementKind kind, UiElementAuthoredState &state) {
        bool changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.state_visible"), &state.visible, context.theme.fonts);
        changed =
            Ui::CheckboxControl(Label(context, "workspace.ui_inspector.state_enabled"), &state.enabled, context.theme.fonts) || changed;
        changed =
            Ui::CheckboxControl(Label(context, "workspace.ui_inspector.state_focusable"), &state.focusable, context.theme.fonts) || changed;
        if (kind != UiElementKind::Toggle && kind != UiElementKind::Slider && kind != UiElementKind::TextInput)
            return changed;
        bool initial = !std::holds_alternative<std::monostate>(state.initial);
        if (Ui::CheckboxControl(Label(context, "workspace.ui_inspector.initial_override"), &initial, context.theme.fonts)) {
            if (!initial)
                state.initial = std::monostate{};
            else if (kind == UiElementKind::Toggle)
                state.initial = false;
            else if (kind == UiElementKind::Slider)
                state.initial = UiControlScalarValues{};
            else
                state.initial = std::string{};
            CaptureInitial(state);
            changed = true;
        }
        if (auto *value = std::get_if<bool>(&state.initial))
            changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.initial_checked"), value, context.theme.fonts) || changed;
        if (auto *value = std::get_if<UiControlScalarValues>(&state.initial)) {
            changed = Number(context, "workspace.ui_inspector.scalar_minimum", value->minimum, scalar_[0]) || changed;
            changed = Number(context, "workspace.ui_inspector.scalar_maximum", value->maximum, scalar_[1]) || changed;
            changed = Number(context, "workspace.ui_inspector.scalar_step", value->step, scalar_[2]) || changed;
            changed = Number(context, "workspace.ui_inspector.scalar_initial", value->value, scalar_[3]) || changed;
        }
        if (auto *value = std::get_if<std::string>(&state.initial))
            if (Text(context, "workspace.ui_inspector.initial_text", initialText_, initialTextValid_)) {
                const auto text = UiActionText::Create(Contents(initialText_));
                initialTextValid_ = text.HasValue();
                if (initialTextValid_)
                    *value = Contents(initialText_);
                changed = true;
            }
        return changed;
    }

    /** @copydoc UiInspectorAuthoringWidgets::DrawAccessibility */
    bool UiInspectorAuthoringWidgets::DrawAccessibility(const EditorGuiContext &context, const UiElementKind kind,
                                                        std::optional<UiElementAccessibility> &value) {
        bool enabled = value.has_value();
        bool changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.accessibility_enabled"), &enabled, context.theme.fonts);
        if (changed) {
            if (enabled)
                value = UiElementAccessibility{.role = DefaultUiElementAccessibilityRole(kind)};
            else
                value.reset();
            name_.Capture(std::nullopt);
            description_.Capture(std::nullopt);
        }
        if (!value)
            return changed;
        changed = Role(context, value->role) || changed;
        changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.accessibility_hidden"), &value->hidden, context.theme.fonts) ||
                  changed;
        changed =
            Ui::CheckboxControl(Label(context, "workspace.ui_inspector.accessibility_read_only"), &value->readOnly, context.theme.fonts) ||
            changed;
        changed =
            Ui::CheckboxControl(Label(context, "workspace.ui_inspector.accessibility_required"), &value->required, context.theme.fonts) ||
            changed;
        changed = name_.Draw(context, "workspace.ui_inspector.accessibility_name", value->name) || changed;
        return description_.Draw(context, "workspace.ui_inspector.accessibility_description", value->description) || changed;
    }

    /** @copydoc UiInspectorAuthoringWidgets::Draw */
    bool UiInspectorAuthoringWidgets::Draw(const EditorGuiContext &context, const UiElementKind kind, UiElementPresentation &value) {
        bool changed = DrawState(context, kind, value.state);
        if (kind == UiElementKind::Text)
            changed = content_.Draw(context, "workspace.ui_inspector.localized_content", value.localizedText) || changed;
        return DrawAccessibility(context, kind, value.accessibility) || changed;
    }

    /** @copydoc UiInspectorAuthoringWidgets::IsValid */
    bool UiInspectorAuthoringWidgets::IsValid() const noexcept {
        const bool scalar = initialKind_ != 2 || std::ranges::all_of(scalar_, [](const auto &text) {
            return text.valid;
        });
        return scalar && initialTextValid_ && content_.IsValid() && name_.IsValid() && description_.IsValid();
    }
}  // namespace Horo::Editor
