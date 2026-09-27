#include "Horo/Editor/EditorConfiguration.h"

#include <cassert>
#include <exception>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr const char *kThemeKey = "editor.theme.active";
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

    /** @copydoc CreateEditorConfigurationService */
    ConfigurationService CreateEditorConfigurationService(const EditorSettings &settings, EngineDataBus *events,
                                                          ConfigurationSchema schema) {
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
        return ConfigurationService{std::move(schema), events};
    }

    /** @copydoc MakeEditorSettingsContribution */
    ModuleConfigurationContribution MakeEditorSettingsContribution(const EditorSettings &settings) {
        const auto setting = [](const char *key, const SettingValueType type, SettingValue value) {
            return SettingDescriptor{.key = SettingKey{key},
                                     .type = type,
                                     .defaultValue = std::move(value),
                                     .scope = SettingScope::User,
                                     .reloadPolicy = ReloadPolicy::NextFrame,
                                     .sensitivity = SettingSensitivity::Public,
                                     .sourcePolicy = ConfigurationSourcePolicy{
                                         .allowedSources = ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment |
                                                           ConfigurationSourceMask::Session | ConfigurationSourceMask::User |
                                                           ConfigurationSourceMask::PackagedProfile}};
        };
        return {.module = ModuleId{"horo.editor.services"},
                .ownerPrefix = "editor",
                .settings = {setting(kThemeKey, SettingValueType::String, std::string{ToConfigurationThemeValue(settings.themePreset)}),
                             setting(kAccentColorKey, SettingValueType::String, settings.accentColorHex),
                             setting(kUiScaleKey, SettingValueType::Integer, static_cast<std::int64_t>(settings.uiScalePercent)),
                             setting(kCodeFontSizeKey, SettingValueType::Integer, static_cast<std::int64_t>(settings.codeFontSizePx))}};
    }

    /** @copydoc MakeEditorAppearanceConfigurationDraft */
    ConfigurationDraft MakeEditorAppearanceConfigurationDraft(const ConfigurationSnapshot &base, const EditorSettings &settings) {
        ConfigurationDraft draft{.baseRevision = base.Revision()};
        draft.proposedValues.try_emplace(SettingKey{kThemeKey}, std::string{ToConfigurationThemeValue(settings.themePreset)});
        draft.proposedValues.try_emplace(SettingKey{kAccentColorKey}, settings.accentColorHex);
        draft.proposedValues.try_emplace(SettingKey{kUiScaleKey}, static_cast<std::int64_t>(settings.uiScalePercent));
        draft.proposedValues.try_emplace(SettingKey{kCodeFontSizeKey}, static_cast<std::int64_t>(settings.codeFontSizePx));
        return draft;
    }
}  // namespace Horo::Editor
