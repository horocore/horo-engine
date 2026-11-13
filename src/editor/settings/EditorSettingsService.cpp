#include "Horo/Editor/EditorSettingsService.h"

#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/EditorServiceErrors.h"
#include "editor/input/EditorInputActions.h"

#include <array>
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

        /** @brief Validates a proposed global profile against the currently admitted project layers before any persistence. */
        Result<void> ValidateEffectiveBindings(const Input::InputBindingProfile &global,
                                               const std::optional<Input::BindingProfilePaths> &paths,
                                               const std::span<const Input::ActionDescriptor> contributions, const bool projectChange) {
            Input::InputBindingProfile effective = global;
            if (paths && projectChange) {
                const std::array defaults{paths->projectDefaults};
                auto project = Input::LoadBindingProfileLayers(defaults);
                if (project.HasError())
                    return Result<void>::Failure(project.ErrorValue());
                auto merged = Input::MergeBindingProfiles(project.Value(), global);
                if (merged.HasError())
                    return Result<void>::Failure(merged.ErrorValue());
                const std::array overrides{paths->projectUser};
                auto user = Input::LoadBindingProfileLayers(overrides);
                if (user.HasError())
                    return Result<void>::Failure(user.ErrorValue());
                merged = Input::MergeBindingProfiles(merged.Value(), user.Value());
                if (merged.HasError())
                    return Result<void>::Failure(merged.ErrorValue());
                effective = std::move(merged).Value();
            }
            const auto validation = Input::ValidateBindingProfile(EditorKeymapActions(effective, contributions), effective);
            if (!validation.IsValid())
                return Result<void>::Failure(SettingsError(SettingsErrors::ValidationFailed, validation.diagnostics.front().message));
            return Result<void>::Success();
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

        if (const auto bindings = ValidateEffectiveBindings(candidate.keymap.profile, m_inputProfilePaths, m_contributedActions,
                                                            candidate.keymap != m_committed.keymap);
            bindings.HasError()) {
            LogCommitFailure("input layer validation", bindings.ErrorValue());
            return Result<EditorSettingsSnapshot>::Failure(bindings.ErrorValue());
        }

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

    /** @copydoc EditorSettingsService::SetInputProfilePaths */
    void EditorSettingsService::SetInputProfilePaths(std::optional<Input::BindingProfilePaths> paths) {
        m_inputProfilePaths = std::move(paths);
    }

}  // namespace Horo::Editor
