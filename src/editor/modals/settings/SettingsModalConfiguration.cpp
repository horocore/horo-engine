#include "Horo/Editor/EditorConfiguration.h"
#include "SettingsModalInternal.h"

#include <array>
#include <cinttypes>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        /** @brief Maps typed source metadata to stable localization keys. */
        const char *SourceText(const ConfigurationSource source) {
            constexpr std::array keys{"configuration.source.invocation", "configuration.source.environment", "configuration.source.session",
                                      "configuration.source.project",    "configuration.source.user",        "configuration.source.profile",
                                      "configuration.source.default"};
            return keys[static_cast<std::size_t>(source)];
        }

        /** @brief Draws ordinary typed values without copying string storage or exposing credential identities. */
        void DrawValue(const SettingValue &value) {
            if (const auto *text = std::get_if<std::string>(&value)) {
                ImGui::PushTextWrapPos(0.0F);
                ImGui::TextUnformatted(text->data(), text->data() + text->size());
                ImGui::PopTextWrapPos();
            } else if (const auto *integer = std::get_if<std::int64_t>(&value)) {
                ImGui::Text("%" PRId64, *integer);
            } else if (const auto *boolean = std::get_if<bool>(&value)) {
                ImGui::TextUnformatted(*boolean ? "true" : "false");
            }
        }

        /** @brief Presents schema-owned names and validation without changing configuration state. */
        void DrawMetadata(const ConfigurationSettingField &field, const EditorGuiContext &context) {
            const auto &descriptor = field.descriptor;
            const auto &title =
                descriptor.label ? context.localization.Get(descriptor.label->namespaceId, descriptor.label->key) : descriptor.key.Value();
            ImGui::PushTextWrapPos(0.0F);
            Ui::SettingGroup(title.c_str(), context.theme.fonts);
            ImGui::PopTextWrapPos();
            Theme::ScopedTextStyle text(context.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            if (descriptor.label)
                ImGui::TextWrapped("%s", descriptor.key.Value().c_str());
            const auto &source = SettingsModal::kTextScope.Text(context.localization, SourceText(field.resolved.source));
            constexpr std::array reloadKeys{"configuration.reload.immediate", "configuration.reload.frame",
                                            "configuration.reload.operation", "configuration.reload.project",
                                            "configuration.reload.process"};
            const auto &reload =
                SettingsModal::kTextScope.Text(context.localization, reloadKeys[static_cast<std::size_t>(descriptor.reloadPolicy)]);
            ImGui::TextWrapped("%s", source.c_str());
            ImGui::TextWrapped("%s", reload.c_str());
            if (const auto *range = std::get_if<SettingIntegerRange>(&descriptor.validation)) {
                const auto &label = SettingsModal::kTextScope.Text(context.localization, "configuration.allowed_range");
                ImGui::TextWrapped("%s: %" PRId64 " – %" PRId64, label.c_str(), range->minimum, range->maximum);
            } else if (const auto *rules = std::get_if<SettingStringRules>(&descriptor.validation)) {
                const auto &label = SettingsModal::kTextScope.Text(context.localization, "configuration.text_bytes");
                ImGui::TextWrapped("%s: %zu – %zu", label.c_str(), rules->minimumBytes, rules->maximumBytes);
            }
        }

        /** @brief Shows committed public values and shadowed sources; sensitive rows never enter this helper. */
        void DrawPublicValues(const ConfigurationSettingField &field, const EditorGuiContext &context) {
            if (field.resolved.location)
                ImGui::TextWrapped("%s", field.resolved.location->source.c_str());
            const auto &effective = SettingsModal::kTextScope.Text(context.localization, "configuration.effective");
            ImGui::TextWrapped("%s", effective.c_str());
            DrawValue(field.resolved.value);
            for (std::size_t index = 1; index < field.resolved.candidates.size(); ++index) {
                const auto &candidate = field.resolved.candidates[index];
                const auto &inactive = SettingsModal::kTextScope.Text(context.localization, "configuration.inactive");
                const auto &source = SettingsModal::kTextScope.Text(context.localization, SourceText(candidate.Source()));
                ImGui::TextWrapped("%s · %s", inactive.c_str(), source.c_str());
                DrawValue(candidate.Value());
            }
        }
    }  // namespace

    /** @copydoc DrawConfiguration */
    void DrawConfiguration(SettingsState &state, const EditorGuiContext &context) {
        using namespace Theme;
        using namespace Ui;
        if (!state.configuration)
            return;
        auto &session = *state.configuration;
        const auto &title = SettingsModal::kTextScope.Text(context.localization, "configuration.title");
        SectionTitle(title.c_str(), context.theme.fonts);
        for (const auto &field : session.Fields()) {
            const auto &key = field.descriptor.key;
            DrawMetadata(field, context);
            if (field.descriptor.sensitivity == SettingSensitivity::Public)
                DrawPublicValues(field, context);
            if (!field.editable) {
                const auto &policy =
                    SettingsModal::kTextScope.Text(context.localization, field.descriptor.sensitivity == SettingSensitivity::SecretReference
                                                                             ? "configuration.sensitive"
                                                                             : "configuration.readonly");
                ImGui::TextWrapped("%s", policy.c_str());
                continue;
            }
            const auto &publicPolicy = SettingsModal::kTextScope.Text(context.localization, "configuration.public");
            ImGui::TextWrapped("%s", publicPolicy.c_str());
            bool changed = false;
            auto *buffer = session.EditBuffer(key);
            const auto &label = SettingsModal::kTextScope.Text(context.localization, "configuration.user_value");
            SettingRow(label.c_str(), nullptr, context.theme.fonts, [&] {
                if (field.descriptor.type == SettingValueType::Boolean) {
                    bool value = *buffer == "true";
                    changed = ToggleControl("##configuration-value", &value, context.theme.fonts, false);
                    if (changed)
                        *buffer = value ? "true" : "false";
                } else {
                    changed = InputTextControl("##configuration-value", *buffer, ConfigurationLimits{}.maximumStringValueBytes,
                                               context.theme.fonts, {.error = field.error.has_value()});
                }
            }, key.Value().c_str(), SettingRowLayout::Stacked);
            if (changed) {
                const auto editedKey = key;
                const auto edited = session.Edit(editedKey, *buffer);
                if (edited.HasError()) {
                    state.configurationFailure = edited.ErrorValue();
                    state.status = SettingsModalStatus::Failure;
                } else {
                    state.configurationFailure.reset();
                    state.status = SettingsModalStatus::None;
                }
                if (edited.HasValue() && editedKey.Value() == "editor.theme.active" && session.Preview()) {
                    const auto &theme = std::get<std::string>(session.Preview()->Get(editedKey));
                    state.appearance.pendingThemeIndex = static_cast<int>(ThemePresetFromConfigurationValue(theme));
                    state.configurationPreviewed = true;
                }
                return;
            }
            if (field.error) {
                const auto &invalid = SettingsModal::kTextScope.Text(context.localization, "configuration.invalid");
                ImGui::PushStyleColor(ImGuiCol_Text, Err());
                ImGui::TextWrapped("%s\n%s", invalid.c_str(), field.error->code.Value().c_str());
                ImGui::PopStyleColor();
            }
            if (session.Preview() && session.Draft().proposedValues.contains(key)) {
                const auto &preview = SettingsModal::kTextScope.Text(context.localization, "configuration.preview");
                ImGui::TextWrapped("%s", preview.c_str());
                DrawValue(session.Preview()->Get(key));
            }
        }
    }
}  // namespace Horo::Editor::SettingsModalInternal
