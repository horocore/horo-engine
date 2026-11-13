#include "Horo/Editor/EditorSettingsService.h"

#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/Assertions.h"
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
        m_configurationRevision = configuration.Snapshot().Revision();
        ApplyEditorConfigurationSnapshot(configuration.Snapshot(), m_committed);
    }

    /** @copydoc EditorSettingsService::Snapshot */
    EditorSettingsSnapshot EditorSettingsService::Snapshot() const noexcept {
        const auto configuration = m_configuration.Snapshot();
        EditorSettings settings = m_committed;
        if (configuration.Revision() != m_configurationRevision)
            ApplyEditorConfigurationSnapshot(configuration, settings);
        return EditorSettingsSnapshot{.settings = std::move(settings),
                                      .revision = m_revision,
                                      .configurationRevision = configuration.Revision()};
    }

    /** @copydoc EditorSettingsService::Commit */
    Result<EditorSettingsSnapshot> EditorSettingsService::Commit(const EditorSettingsDraft &draft,
                                                                 const ConfigurationDraft *configurationDraft) {
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

        auto appearanceDraft = MakeEditorAppearanceConfigurationDraft(m_configuration.Snapshot(), candidate);
        appearanceDraft.baseRevision = draft.baseConfigurationRevision.value_or(m_configurationRevision);
        const auto previousAppearance = MakeEditorAppearanceConfigurationDraft(m_configuration.Snapshot(), Snapshot().settings);
        std::erase_if(appearanceDraft.proposedValues, [&](const auto &entry) {
            return previousAppearance.proposedValues.at(entry.first) == entry.second;
        });
        if (configurationDraft) {
            if (configurationDraft->source != ConfigurationSource::User)
                return Result<EditorSettingsSnapshot>::Failure(MakeError(SettingsErrors::ValidationFailed));
            if (configurationDraft->baseRevision != appearanceDraft.baseRevision) {
                const auto legacy = m_configuration.Validate(appearanceDraft);
                if (legacy.HasError())
                    return Result<EditorSettingsSnapshot>::Failure(legacy.ErrorValue());
                const auto generic = m_configuration.Validate(*configurationDraft);
                if (generic.HasError())
                    return Result<EditorSettingsSnapshot>::Failure(generic.ErrorValue());
            }
            for (const auto &[key, value] : configurationDraft->proposedValues) {
                const auto [entry, inserted] = appearanceDraft.proposedValues.emplace(key, value);
                if (!inserted && entry->second != value)
                    return Result<EditorSettingsSnapshot>::Failure(MakeError(SettingsErrors::ValidationFailed));
            }
        }
        auto prepared = m_configuration.Prepare(appearanceDraft);
        if (prepared.HasError())
            return Result<EditorSettingsSnapshot>::Failure(prepared.ErrorValue());
        auto transaction = std::move(prepared).Value();
        ApplyEditorConfigurationSnapshot(transaction.Candidate(), candidate);
        return PersistAndActivate(std::move(candidate), std::move(transaction));
    }

    /** @copydoc EditorSettingsService::Configuration */
    ConfigurationService &EditorSettingsService::Configuration() noexcept {
        return m_configuration;
    }

    /** @copydoc EditorSettingsService::CommitConfiguration */
    Result<EditorSettingsSnapshot> EditorSettingsService::CommitConfiguration(const ConfigurationDraft &draft) {
        if (draft.source != ConfigurationSource::User)
            return Result<EditorSettingsSnapshot>::Failure(MakeError(SettingsErrors::ValidationFailed));
        auto prepared = m_configuration.Prepare(draft);
        if (prepared.HasError())
            return Result<EditorSettingsSnapshot>::Failure(prepared.ErrorValue());
        auto transaction = std::move(prepared).Value();
        EditorSettings candidate = m_committed;
        ApplyEditorConfigurationSnapshot(transaction.Candidate(), candidate);
        return PersistAndActivate(std::move(candidate), std::move(transaction));
    }

    /** @copydoc EditorSettingsService::PersistAndActivate */
    Result<EditorSettingsSnapshot> EditorSettingsService::PersistAndActivate(EditorSettings candidate,
                                                                             ConfigurationService::PreparedCommit transaction) {
        if (!ValidateEditorSettings(candidate))
            return Result<EditorSettingsSnapshot>::Failure(MakeError(SettingsErrors::ValidationFailed));
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

        const auto configuration = transaction.Candidate();
        EditorSettingsDocument document{.settings = candidate};
        if (std::string persistenceError; !SaveEditorSettingsDocument(&document, &persistenceError, &configuration)) {
            Error error = SettingsError(SettingsErrors::PersistenceFailed, std::move(persistenceError));
            LogCommitFailure("persistence", error);
            return Result<EditorSettingsSnapshot>::Failure(std::move(error));
        }

        // This owner-thread operation invokes no localization callbacks between Prepare and this shared-pointer transfer.
        if (languageChanged)
            HORO_INVARIANT_MSG(m_localization.ActivatePrepared(),
                               "Prepared localization must survive the synchronous settings publication.");

        m_committed = std::move(document.settings);
        m_configurationRevision = configuration.Revision();
        ++m_revision;
        const EditorSettingsSnapshot activated{.settings = m_committed,
                                               .revision = m_revision,
                                               .configurationRevision = m_configurationRevision};
        // Publish the editor projection before configuration callbacks can reenter either authority.
        // This fresh, privately owned transaction cannot lose its writer reservation before Commit.
        const auto result = transaction.Commit();
        HORO_INVARIANT_MSG(result.HasValue(), "A prepared editor configuration transaction must activate after durable publication.");
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
