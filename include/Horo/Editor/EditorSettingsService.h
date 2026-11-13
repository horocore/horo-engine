#pragma once

#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/Result.h"

#include <optional>

namespace Horo::Editor {
    class EditorDataBus;
    class LocalizationService;

    /** @brief Immutable value snapshot returned by the editor settings authority. */
    struct EditorSettingsSnapshot {
        EditorSettings settings{};
        std::uint64_t revision = 0;
        std::uint64_t configurationRevision = 0;

        bool operator==(const EditorSettingsSnapshot &) const = default;
    };

    /** @brief A settings edit formed against one committed editor settings revision. */
    struct EditorSettingsDraft {
        std::uint64_t baseRevision = 0;
        EditorSettings settings{};
        std::optional<std::uint64_t> baseConfigurationRevision;
    };

    /**
     * @brief Editor-owned authority for committed user settings and their appearance adapter.
     *
     * A commit validates the draft and rejects a stale base revision. It persists the
     * complete settings document, applies the associated Foundation appearance draft,
     * activates one new immutable editor snapshot, then publishes exactly one
     * committed editor-session notification.
     */
    class EditorSettingsService {
    public:
        /**
         * @brief Constructs the authority from the already loaded editor settings.
         * @param initialSettings Initial validated editor settings.
         * @param configuration Borrowed Foundation configuration appearance adapter.
         * @param events Borrowed editor-session event bus.
         * @param localization Borrowed owner-lane localization authority.
         */
        EditorSettingsService(EditorSettings initialSettings, ConfigurationService &configuration, EditorDataBus &events,
                              LocalizationService &localization);

        /** @brief Returns a stable value copy of the currently committed editor settings. */
        [[nodiscard]] EditorSettingsSnapshot Snapshot() const noexcept;

        /**
         * @brief Validates, persists, activates, and reports one editor settings transaction.
         * @param draft Proposed settings and the revision they were based on.
         * @param configurationDraft Optional descriptor-driven user edits from the same captured configuration revision.
         * @return The newly committed snapshot, or a typed validation, stale, persistence,
         *         or configuration error. Failure leaves this authority unchanged.
         */
        [[nodiscard]] Result<EditorSettingsSnapshot> Commit(const EditorSettingsDraft &draft,
                                                            const ConfigurationDraft *configurationDraft = nullptr);
        /** @brief Returns the explicitly composed shared configuration authority. @return Borrowed service with the same owner lifetime. */
        [[nodiscard]] ConfigurationService &Configuration() noexcept;
        /** @brief Commits one descriptor-driven user draft through durable editor persistence and shared publication.
         * @param draft Captured user-source transaction. @return Committed editor projection or typed failure. */
        [[nodiscard]] Result<EditorSettingsSnapshot> CommitConfiguration(const ConfigurationDraft &draft);

        /** @brief Publishes owner-lane live module command metadata; absent modules retain dormant user bindings.
         * @param actions Inert command descriptors, copied into the authority.
         */
        void SetContributedInputActions(std::vector<Input::ActionDescriptor> actions);
        /** @brief Returns the current live module command descriptors until the next publication. */
        [[nodiscard]] std::span<const Input::ActionDescriptor> ContributedInputActions() const noexcept;

    private:
        /** @brief Persists the combined prepared configuration and editor projection before publishing either authority.
         * @param candidate Validated editor projection. @param transaction Exclusive prepared shared configuration write.
         * @return Published projection, or a failure before activation. */
        [[nodiscard]] Result<EditorSettingsSnapshot> PersistAndActivate(EditorSettings candidate,
                                                                        ConfigurationService::PreparedCommit transaction);
        std::vector<Input::ActionDescriptor> m_contributedActions;
        EditorSettings m_committed;
        std::uint64_t m_revision = 0;
        std::uint64_t m_configurationRevision = 0;
        ConfigurationService &m_configuration;
        EditorDataBus &m_events;
        LocalizationService &m_localization;
    };
}  // namespace Horo::Editor
