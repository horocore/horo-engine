#pragma once

#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/ModuleHost.h"

#include <functional>
#include <string_view>

namespace Horo::Editor {
    class EditorSettingsService;

    /** @brief Explicit source-owned persistence for the shared descriptor editing session.
     * @details Tooling-only callbacks run synchronously on the owner thread without reentering configuration writers.
     * They capture revocable owner borrows, not a project lifetime. Persist receives a fully validated candidate while
     * ConfigurationService::Prepare reserves the writer gate. Failure leaves active configuration unchanged; a host
     * encountering irreversible filesystem publication must retain its receipt and require reconciliation before retry.
     */
    struct ConfigurationSettingsPersistence final {
        ConfigurationSource source{ConfigurationSource::Project};
        std::vector<SettingKey> keys; /**< Required finite exact owner keys, taken from its inert contribution rather than UI wiring. */
        std::function<Result<void>()> checkAvailable; /**< Required revocation check before every edit, preview and apply. */
        std::function<Result<void>(const ConfigurationSnapshot &)> validate; /**< Required typed domain/capability validation. */
        std::function<Result<void>(const ConfigurationSnapshot &)> persist;  /**< Required bounded durable source transaction. */
    };

    /** @brief One descriptor-driven settings field with safe provenance and typed inline failure. */
    struct ConfigurationSettingField {
        SettingDescriptor descriptor;
        ResolvedSetting resolved;
        bool editable = false;
        std::string text;
        std::optional<Error> error;
    };

    /** @brief Owner-thread modal transaction over the shared configuration authority. */
    class ConfigurationSettingsSession final {
    public:
        /** @brief Captures the authority and its user-editable descriptors. @param configuration Borrowed authority outliving this session.
         */
        explicit ConfigurationSettingsSession(ConfigurationService &configuration);
        /** @brief Captures the editor's durable settings operation. @param settings Borrowed owner outliving this modal session. */
        explicit ConfigurationSettingsSession(EditorSettingsService &settings);
        /** @brief Captures an explicit source transaction without introducing another draft model.
         * @param configuration Borrowed authority outliving this session and every synchronous callback.
         * @param persistence Complete source policy, revocable availability, validation and persistence callbacks.
         * @details Missing callbacks fail closed. Fields admit edits only when their descriptor allows the exact source.
         * Cancel never invokes persistence; Apply reserves one validated configuration generation across persistence.
         */
        ConfigurationSettingsSession(ConfigurationService &configuration, ConfigurationSettingsPersistence persistence);
        /** @brief Returns stable field order until Reload or Cancel. @return Session-owned fields and safe provenance. */
        [[nodiscard]] std::span<const ConfigurationSettingField> Fields() const noexcept;
        /** @brief Returns the active draft. @return Borrowed source transaction with captured base revision. */
        [[nodiscard]] const ConfigurationDraft &Draft() const noexcept;
        /** @brief Returns the latest immutable preview. @return Session-owned preview, absent after a failed edit or cancel. */
        [[nodiscard]] const std::optional<ConfigurationSnapshot> &Preview() const noexcept;
        /** @brief Returns the latest transaction failure. @return Safe typed failure, including configuration.draft_stale. */
        [[nodiscard]] const std::optional<Error> &Failure() const noexcept;
        /** @brief Validates one text edit using the declared primitive type. @param key User-editable field. @param text Proposed text.
         * @return Success or typed inline failure without changing active configuration. */
        [[nodiscard]] Result<void> Edit(const SettingKey &key, std::string text);
        /** @brief Borrows only a field's transient input buffer. @param key Editable field. @return Buffer or null.
         * @note Call Edit after a control changes this buffer. No buffer mutation changes the draft or authority. */
        [[nodiscard]] std::string *EditBuffer(const SettingKey &key) noexcept;
        /** @brief Applies every validated field atomically, refreshing stale drafts for review. @return Commit or typed failure. */
        [[nodiscard]] Result<void> Apply();
        /** @brief Previews descriptor defaults for all eligible fields without changing committed state.
         * @return Complete default preview or typed stale/validation failure. */
        [[nodiscard]] Result<void> RestoreDefaults();
        /** @brief Captures current authority values and discards proposed edits and previews. */
        void Reload();
        /** @brief Discards all edits and preview state without publishing a configuration change. */
        void Cancel();

    private:
        /** @brief Checks source-owner revocation without touching durable state. @return Original host error or success. */
        [[nodiscard]] Result<void> CheckAvailable() const;
        /** @brief Validates source semantics after Foundation resolution. @param snapshot Complete preview.
         * @return Original typed domain error or success. */
        [[nodiscard]] Result<void> ValidateSource(const ConfigurationSnapshot &snapshot) const;
        ConfigurationService &configuration_;
        EditorSettingsService *settings_ = nullptr;
        std::optional<ConfigurationSettingsPersistence> persistence_;
        ConfigurationDraft draft_;
        std::vector<ConfigurationSettingField> fields_;
        std::optional<ConfigurationSnapshot> preview_;
        std::optional<Error> failure_;
    };

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

    /** @brief Completes and seals the editor schema without constructing a configuration authority.
     * @param settings Persisted appearance defaults for descriptors absent from the module schema.
     * @param schema Active module schema; standalone callers may supply an empty schema.
     * @return Owned sealed schema suitable for direct ConfigurationService construction.
     * @pre Module descriptor registration is complete; schema validation failure is an invariant violation.
     */
    [[nodiscard]] ConfigurationSchema CreateEditorConfigurationSchema(const EditorSettings &settings, ConfigurationSchema schema = {});

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

    /** @brief Projects authoritative appearance values into the editor's typed presentation model.
     * @param snapshot Captured shared configuration. @param settings In/out editor settings retaining other domain fields. */
    void ApplyEditorConfigurationSnapshot(const ConfigurationSnapshot &snapshot, EditorSettings &settings);

}  // namespace Horo::Editor
