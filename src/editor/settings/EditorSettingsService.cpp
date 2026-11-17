#include "Horo/Editor/EditorSettingsService.h"

#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/EditorServiceErrors.h"
#include "editor/input/EditorInputActions.h"

#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error SettingsError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        void LogCommitFailure(const char *stage, const Error &error) {
            LOG_ERROR("editor.settings", "Settings commit failed at %s: code=%s message=%s", stage, error.code.Value().c_str(),
                      error.message.c_str());
        }
    }  // namespace

    /** @copydoc EditorSettingsService::EditorSettingsService */
    EditorSettingsService::EditorSettingsService(EditorSettings initialSettings, ConfigurationService &configuration, EditorDataBus &events,
                                                 LocalizationService &localization)
        : m_committed(std::move(initialSettings)), m_configuration(configuration), m_events(events), m_localization(localization) {
        (void)ValidateEditorSettings(m_committed, nullptr);
    }

    /** @copydoc EditorSettingsService::Snapshot */
    EditorSettingsSnapshot EditorSettingsService::Snapshot() const noexcept {
        return EditorSettingsSnapshot{.settings = m_committed, .revision = m_revision};
    }

    /** @copydoc EditorSettingsService::Commit */
    Result<EditorSettingsSnapshot> EditorSettingsService::Commit(const EditorSettingsDraft &draft) {
        if (draft.baseRevision != m_revision) {
            Error error = SettingsError(SettingsErrors::DraftStale, "Editor settings draft is stale.");
            LOG_WARN("editor.settings", "Settings commit rejected as stale: draft_revision=%llu current_revision=%llu",
                     static_cast<unsigned long long>(draft.baseRevision), static_cast<unsigned long long>(m_revision));
            return Result<EditorSettingsSnapshot>::Failure(std::move(error));
        }

        EditorSettings candidate = draft.settings;
        if (std::string validationError; !ValidateEditorSettings(candidate, &validationError)) {
            Error error = SettingsError(SettingsErrors::ValidationFailed, std::move(validationError));
            LogCommitFailure("editor validation", error);
            return Result<EditorSettingsSnapshot>::Failure(std::move(error));
        }

        if (!Input::ValidateBindingProfile(EditorKeymapActions(candidate.keymap.profile, m_contributedActions), candidate.keymap.profile)
                 .IsValid())
            return Result<EditorSettingsSnapshot>::Failure(
                SettingsError(SettingsErrors::ValidationFailed, "Editor keymap conflicts with a live module command."));

        const bool languageChanged = candidate.languageTag != m_committed.languageTag;
        if (languageChanged) {
            const auto locale = LocaleTag::Parse(candidate.languageTag);
            LocalizationError localizationError;
            if (!locale.has_value() || !m_localization.Prepare(*locale, &localizationError)) {
                Error error = SettingsError(SettingsErrors::ValidationFailed, std::move(localizationError.message));
                LogCommitFailure("localization preparation", error);
                return Result<EditorSettingsSnapshot>::Failure(std::move(error));
            }
        }

        const ConfigurationSnapshot configurationSnapshot = m_configuration.Snapshot();
        const ConfigurationDraft appearanceDraft = MakeEditorAppearanceConfigurationDraft(configurationSnapshot, candidate);
        if (const Result<void> result = m_configuration.Validate(appearanceDraft); result.HasError()) {
            LogCommitFailure("configuration validation", result.ErrorValue());
            return Result<EditorSettingsSnapshot>::Failure(result.ErrorValue());
        }

        EditorSettingsDocument document{.settings = candidate};
        if (std::string persistenceError; !SaveEditorSettingsDocument(&document, &persistenceError)) {
            Error error = SettingsError(SettingsErrors::PersistenceFailed, std::move(persistenceError));
            LogCommitFailure("persistence", error);
            return Result<EditorSettingsSnapshot>::Failure(std::move(error));
        }

        if (languageChanged) {
            LocalizationError localizationError;
            if (!m_localization.ActivatePrepared(&localizationError)) {
                Error error = SettingsError(SettingsErrors::ValidationFailed, std::move(localizationError.message));
                LogCommitFailure("localization activation", error);
                return Result<EditorSettingsSnapshot>::Failure(std::move(error));
            }
        }

        if (const Result<void> result = m_configuration.Commit(appearanceDraft); result.HasError()) {
            LogCommitFailure("configuration activation", result.ErrorValue());
            return Result<EditorSettingsSnapshot>::Failure(result.ErrorValue());
        }

        m_committed = std::move(document.settings);
        ++m_revision;
        const EditorSettingsSnapshot activated = Snapshot();
        m_events.Publish(EditorSettingsChangedEvent{
            .revision = activated.revision,
            .phase = SettingsChangePhase::Committed,
            .changedDomains = SettingsDomain::All,
        });
        LOG_INFO("editor.settings", "Committed editor settings revision=%llu grid_overlay=%s language=%s.",
                 static_cast<unsigned long long>(activated.revision), activated.settings.gridOverlay ? "enabled" : "disabled",
                 activated.settings.languageTag.c_str());
        return Result<EditorSettingsSnapshot>::Success(activated);
    }

    /** @copydoc EditorSettingsService::SetContributedInputActions */
    void EditorSettingsService::SetContributedInputActions(std::vector<Input::ActionDescriptor> actions) {
        m_contributedActions = std::move(actions);
    }

    /** @copydoc EditorSettingsService::ContributedInputActions */
    std::span<const Input::ActionDescriptor> EditorSettingsService::ContributedInputActions() const noexcept {
        return m_contributedActions;
    }

}  // namespace Horo::Editor
