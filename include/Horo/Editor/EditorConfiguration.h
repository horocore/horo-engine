#pragma once

#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/ModuleHost.h"

#include <string_view>

namespace Horo::Editor {

    /**
     * @file EditorConfiguration.h
     * @brief Editor-owned mappings between persisted editor settings and Foundation configuration.
     */

    /**
     * @brief Converts an editor theme preset to its stable Foundation configuration value.
     * @param preset Editor theme preset.
     * @return Stable string stored under `editor.theme.active`.
     */
    std::string_view ToConfigurationThemeValue(EditorThemePreset preset) noexcept;

    /**
     * @brief Converts a stable Foundation theme value to an editor theme preset.
     * @param value Stable string from `editor.theme.active`.
     * @return Matching editor theme preset, or HoroDark for an unknown value.
     */
    EditorThemePreset ThemePresetFromConfigurationValue(std::string_view value) noexcept;

    /** @brief Returns the editor settings metadata supplied at module composition.
     * @param settings Persisted settings used as initial configuration defaults.
     * @return Owned inert settings contribution for `horo.editor.services`.
     */
    [[nodiscard]] ModuleConfigurationContribution MakeEditorSettingsContribution(const EditorSettings &settings);

    /**
     * @brief Builds the editor appearance configuration authority from persisted defaults.
     * @param settings Persisted editor settings whose appearance values initialize the schema.
     * @param events Optional process-owned engine event bus notified on successful commits.
     * @param schema Active module settings schema; standalone callers may pass an empty schema.
     * @return Composition-root-owned configuration service.
     */
    ConfigurationService CreateEditorConfigurationService(const EditorSettings &settings, EngineDataBus *events = nullptr,
                                                          ConfigurationSchema schema = {});

    /**
     * @brief Maps an editor appearance snapshot to one Foundation configuration draft.
     * @param base Snapshot against which the draft will be committed.
     * @param settings Persisted editor settings to map.
     * @return Draft containing all editor appearance keys atomically.
     */
    ConfigurationDraft MakeEditorAppearanceConfigurationDraft(const ConfigurationSnapshot &base, const EditorSettings &settings);

}  // namespace Horo::Editor
