#include "Horo/Editor/EditorConfiguration.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr const char *kThemeKey = "editor.theme.active";
        constexpr const char *kPackageThemeKey = "editor.theme.package_id";
        constexpr const char *kAccentColorKey = "editor.appearance.accent_color";
        constexpr const char *kUiScaleKey = "editor.appearance.ui_scale_percent";
        constexpr const char *kCodeFontSizeKey = "editor.appearance.code_font_size_px";

        void RegisterAppearanceDescriptor(ConfigurationSchema &schema, const SettingDescriptor &descriptor) {
            const Result<void> registered = schema.Register(descriptor);
            assert(registered.HasValue());
            if (registered.HasError()) {
                std::terminate();
            }
        }

        /** @brief Builds one inert user appearance descriptor with canonical source admission. */
        SettingDescriptor AppearanceSetting(const char *key, const SettingValueType type, SettingValue value,
                                            SettingValidation validation = {}, std::string label = {}) {
            SettingDescriptor descriptor{.key = SettingKey{key},
                                         .type = type,
                                         .defaultValue = std::move(value),
                                         .scope = SettingScope::User,
                                         .reloadPolicy = ReloadPolicy::NextFrame,
                                         .sensitivity = SettingSensitivity::Public,
                                         .sourcePolicy =
                                             ConfigurationSourcePolicy{.allowedSources = ConfigurationSourceMask::Invocation |
                                                                                         ConfigurationSourceMask::Environment |
                                                                                         ConfigurationSourceMask::Session |
                                                                                         ConfigurationSourceMask::User |
                                                                                         ConfigurationSourceMask::PackagedProfile},
                                         .validation = std::move(validation)};
            if (!label.empty())
                descriptor.label = SettingLabelKey{.namespaceId = "editor", .key = std::move(label)};
            return descriptor;
        }
    }  // namespace

    /** @copydoc ToConfigurationThemeValue */
    std::string_view ToConfigurationThemeValue(const EditorThemePreset preset) noexcept {
        using enum EditorThemePreset;
        switch (preset) {
            case Midnight:
                return "midnight";
            case Light:
                return "light";
            case HoroDark:
            default:
                return "horo_dark";
        }
    }

    /** @copydoc ThemePresetFromConfigurationValue */
    EditorThemePreset ThemePresetFromConfigurationValue(const std::string_view value) noexcept {
        using enum EditorThemePreset;
        if (value == "midnight") {
            return Midnight;
        }
        if (value == "light") {
            return Light;
        }
        return HoroDark;
    }

    /** @copydoc CreateEditorConfigurationSchema */
    ConfigurationSchema CreateEditorConfigurationSchema(const EditorSettings &settings, ConfigurationSchema schema) {
        const ModuleConfigurationContribution contribution = MakeEditorSettingsContribution(settings);
        for (const SettingDescriptor &descriptor : contribution.settings) {
            if (schema.FindDescriptor(descriptor.key) == nullptr)
                RegisterAppearanceDescriptor(schema, descriptor);
        }

        const Result<void> sealed = schema.Seal();
        assert(sealed.HasValue());
        if (sealed.HasError()) {
            std::terminate();
        }
        return schema;
    }

    /** @copydoc CreateEditorConfigurationService */
    ConfigurationService CreateEditorConfigurationService(const EditorSettings &settings, EngineDataBus *events,
                                                          ConfigurationSchema schema) {
        ConfigurationResolutionRequest initial;
        for (const auto &descriptor : MakeEditorSettingsContribution(settings).settings) {
            if (ConfigurationSchema::AllowsSource(descriptor, ConfigurationSource::User))
                initial.user.emplace(descriptor.key, ConfigurationInputValue{.value = descriptor.defaultValue,
                                                                             .location = SourceLocation{.source = "editor-user"}});
        }
        return ConfigurationService{CreateEditorConfigurationSchema(settings, std::move(schema)), events, initial};
    }

    /** @copydoc MakeEditorSettingsContribution */
    ModuleConfigurationContribution MakeEditorSettingsContribution(const EditorSettings &settings) {
        ModuleConfigurationContribution
            contribution{.module = ModuleId{"horo.editor.services"},
                         .ownerPrefix = "editor",
                         .settings = {AppearanceSetting(kThemeKey, SettingValueType::String,
                                                        std::string{ToConfigurationThemeValue(settings.themePreset)},
                                                        SettingStringRules{.choices = {"horo_dark", "midnight", "light"}},
                                                        "settings.appearance.color_theme"),
                                      AppearanceSetting(kPackageThemeKey, SettingValueType::String, settings.themeId,
                                                        SettingStringRules{.maximumBytes = 256,
                                                                           .allowedCharacters = "abcdefghijklmnopqrstuvwxyz0123456789._-"},
                                                        "settings.configuration.theme_package"),
                                      AppearanceSetting(kAccentColorKey, SettingValueType::String, settings.accentColorHex,
                                                        SettingStringRules{.minimumBytes = 7,
                                                                           .maximumBytes = 7,
                                                                           .requiredPrefix = "#",
                                                                           .allowedCharacters = "0123456789abcdefABCDEF"},
                                                        "settings.appearance.accent_color"),
                                      AppearanceSetting(kUiScaleKey, SettingValueType::Integer,
                                                        static_cast<std::int64_t>(settings.uiScalePercent), SettingIntegerRange{75, 200},
                                                        "settings.appearance.ui_scale"),
                                      AppearanceSetting(kCodeFontSizeKey, SettingValueType::Integer,
                                                        static_cast<std::int64_t>(settings.codeFontSizePx), SettingIntegerRange{14, 24},
                                                        "settings.appearance.code_font_size")}};
        contribution.settings.push_back(
            AppearanceSetting("editor.appearance.code_font_family", SettingValueType::String, settings.codeFontFamily,
                              SettingStringRules{.minimumBytes = 1, .maximumBytes = 128}, "settings.code_editor.font_family"));
        for (const auto &entry : CodeEditorBooleanSettings())
            contribution.settings.push_back(AppearanceSetting((std::string{"editor.code."} + entry.key).c_str(), SettingValueType::Boolean,
                                                              settings.codeEditor.*entry.member, {},
                                                              std::string{"settings.code_editor."} + entry.key));
        for (const auto &entry : CodeEditorIntegerSettings())
            contribution.settings.push_back(AppearanceSetting((std::string{"editor.code."} + entry.key).c_str(), SettingValueType::Integer,
                                                              static_cast<std::int64_t>(settings.codeEditor.*entry.member),
                                                              SettingIntegerRange{entry.minimum, entry.maximum},
                                                              std::string{"settings.code_editor."} + entry.key));
        contribution.settings.push_back(
            {.key = SettingKey{"editor.theme.override_path"},
             .type = SettingValueType::String,
             .defaultValue = std::string{},
             .scope = SettingScope::Invocation,
             .reloadPolicy = ReloadPolicy::ProcessRestart,
             .sensitivity = SettingSensitivity::Public,
             .sourcePolicy =
                 ConfigurationSourcePolicy{.allowedSources = ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment},
             .validation = SettingStringRules{.maximumBytes = 1024},
             .label = SettingLabelKey{.namespaceId = "editor", .key = "settings.configuration.theme_override"}});
        contribution.environmentBindings.push_back({.key = SettingKey{"editor.theme.override_path"}, .variable = "HORO_THEME_OVERRIDE"});
        return contribution;
    }

    /** @copydoc MakeEditorAppearanceConfigurationDraft */
    ConfigurationDraft MakeEditorAppearanceConfigurationDraft(const ConfigurationSnapshot &base, const EditorSettings &settings) {
        ConfigurationDraft draft{.baseRevision = base.Revision(), .source = ConfigurationSource::User};
        draft.proposedValues.try_emplace(SettingKey{kThemeKey}, std::string{ToConfigurationThemeValue(settings.themePreset)});
        draft.proposedValues.try_emplace(SettingKey{kPackageThemeKey}, settings.themeId);
        draft.proposedValues.try_emplace(SettingKey{kAccentColorKey}, settings.accentColorHex);
        draft.proposedValues.try_emplace(SettingKey{kUiScaleKey}, static_cast<std::int64_t>(settings.uiScalePercent));
        draft.proposedValues.try_emplace(SettingKey{kCodeFontSizeKey}, static_cast<std::int64_t>(settings.codeFontSizePx));
        draft.proposedValues.try_emplace(SettingKey{"editor.appearance.code_font_family"}, settings.codeFontFamily);
        for (const auto &entry : CodeEditorBooleanSettings())
            draft.proposedValues.try_emplace(SettingKey{std::string{"editor.code."} + entry.key}, settings.codeEditor.*entry.member);
        for (const auto &entry : CodeEditorIntegerSettings())
            draft.proposedValues.try_emplace(SettingKey{std::string{"editor.code."} + entry.key},
                                             static_cast<std::int64_t>(settings.codeEditor.*entry.member));
        return draft;
    }

    /** @copydoc ApplyEditorConfigurationSnapshot */
    void ApplyEditorConfigurationSnapshot(const ConfigurationSnapshot &snapshot, EditorSettings &settings) {
        const auto text = [&](const char *key, std::string &target) {
            const auto *resolved = snapshot.FindResolved(SettingKey{key});
            if (resolved) {
                if (const auto *value = std::get_if<std::string>(&resolved->value))
                    target = *value;
            }
        };
        const auto integer = [&](const char *key, int &target) {
            const auto *resolved = snapshot.FindResolved(SettingKey{key});
            if (resolved) {
                if (const auto *value = std::get_if<std::int64_t>(&resolved->value))
                    target = static_cast<int>(
                        std::clamp(*value, std::int64_t{std::numeric_limits<int>::min()}, std::int64_t{std::numeric_limits<int>::max()}));
            }
        };
        if (const auto theme = snapshot.Find(SettingKey{kThemeKey}); theme && std::holds_alternative<std::string>(*theme))
            settings.themePreset = ThemePresetFromConfigurationValue(std::get<std::string>(*theme));
        text(kPackageThemeKey, settings.themeId);
        text(kAccentColorKey, settings.accentColorHex);
        text("editor.appearance.code_font_family", settings.codeFontFamily);
        integer(kUiScaleKey, settings.uiScalePercent);
        integer(kCodeFontSizeKey, settings.codeFontSizePx);
        for (const auto &entry : CodeEditorBooleanSettings()) {
            const auto *resolved = snapshot.FindResolved(SettingKey{std::string{"editor.code."} + entry.key});
            if (resolved) {
                if (const auto *value = std::get_if<bool>(&resolved->value))
                    settings.codeEditor.*entry.member = *value;
            }
        }
        for (const auto &entry : CodeEditorIntegerSettings())
            integer((std::string{"editor.code."} + entry.key).c_str(), settings.codeEditor.*entry.member);
    }
}  // namespace Horo::Editor
