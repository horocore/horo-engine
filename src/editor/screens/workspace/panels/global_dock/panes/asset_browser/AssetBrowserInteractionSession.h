#pragma once

#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /** @brief Asset Browser layout selected by the grid/list switch. */
    enum class AssetBrowserViewMode : std::uint8_t {
        Grid,
        List,
    };

    /** @brief Unsaved first Blackboard source and its user-authored initial key. */
    struct BlackboardCreationDraft final {
        BlackboardCreationRequest request;
        std::string numericDefault;
    };

    /** @brief Draft of the typed point-lattice creation dialog. Signed controls preserve invalid draft values. */
    struct EnvironmentQueryCreationDraft final {
        std::string directory;
        std::array<char, AI::EnvironmentQuerySchemaLimits::DisplayNameBytes + 1> name{};
        int stepMillimeters{1000};
        int outerSteps{1};
        int innerSteps{};
        int maximumResults{9};
        bool annulus{};
        bool distance{};
        int radiusMillimeters{1500};
        bool preferNearer{true};
    };

    /** @brief Cross-frame presentation state for one Content Browser panel. */
    struct AssetBrowserInteractionState {
        std::optional<ContentBrowserEntry> popupEntry;
        std::string selectedAbsolutePath;
        std::array<char, 160> search{};
        std::string assetTypeFilter;
        ContentBrowserSortField sortField{ContentBrowserSortField::Name};
        ContentBrowserSortDirection sortDirection{ContentBrowserSortDirection::Ascending};
        AssetBrowserViewMode viewMode{AssetBrowserViewMode::Grid};
        std::array<char, 256> renameBuffer{};
        std::array<char, 256> createFolderBuffer{};
        bool openAssetInfo{false};
        bool openRename{false};
        bool openDeleteConfirmation{false};
        bool openCreateFolder{false};
        EnvironmentQueryCreationDraft createQuery;
        bool openCreateQuery{};
        BlackboardCreationDraft createBlackboard;
        bool openCreateBlackboard{};
        StateMachineCreationRequest createStateMachine;
        bool openCreateStateMachine{};
        TaskDecisionCreationRequest createTaskDecision;
        float utilityMinimum{};
        float utilityMaximum{1};
        bool openCreateTaskDecision{};
    };

    /**
     * @brief Owns Content Browser interaction state and typed command reduction.
     *
     * ImGui code reports semantic actions to this session. Filesystem mutation
     * remains exclusively owned by the workspace controller.
     */
    class AssetBrowserInteractionSession {
    public:
        /** @brief Returns mutable presentation state used by widgets. */
        [[nodiscard]] AssetBrowserInteractionState &State() noexcept;

        /** @brief Returns the current presentation state. */
        [[nodiscard]] const AssetBrowserInteractionState &State() const noexcept;

        /** @brief Projects filtered and sorted entry indices. */
        [[nodiscard]] std::vector<std::size_t> ProjectEntries(const ContentBrowserDirectory &directory) const;

        /** @brief Selects one absolute entry path. */
        void Select(std::string_view absolutePath);

        /** @brief Prepares the rename workflow for an entry. */
        void OpenRename(const ContentBrowserEntry &entry);

        /** @brief Prepares the information workflow for an entry. */
        void OpenInfo(const ContentBrowserEntry &entry);

        /** @brief Prepares the destructive confirmation workflow for an entry. */
        void OpenDelete(const ContentBrowserEntry &entry);

        /** @brief Prepares the create-folder workflow. */
        void OpenCreateFolder();

        /** @brief Captures the selected folder and exact task catalog publication for first source creation. */
        void OpenCreateTaskDecision(std::string_view directory, AI::DecisionPlanKind family, std::uint64_t catalogRevision);
        /** @brief Checks the explicit native task, schema and Utility score input. */
        [[nodiscard]] static bool CanConfirmTaskDecision(const TaskDecisionCreationRequest &draft) noexcept;
        /** @brief Reduces a valid task-based Decision draft to a normal Project command. */
        [[nodiscard]] static std::optional<EditorWorkspaceViewCommandData> ConfirmTaskDecision(const TaskDecisionCreationRequest &draft);

        /** @brief Captures the selected folder for the initial State Machine modal. */
        void OpenCreateStateMachine(std::string_view directory);

        /** @brief Checks that a registered schema and terminal outcome are explicitly selected. */
        [[nodiscard]] static bool CanConfirmStateMachine(const StateMachineCreationRequest &draft) noexcept;
        /** @brief Reduces a complete State Machine request to the normal Project command. */
        [[nodiscard]] static std::optional<EditorWorkspaceViewCommandData> ConfirmStateMachine(const StateMachineCreationRequest &draft);

        /** @brief Captures the selected Assets directory for the first schema modal. */
        void OpenCreateBlackboard(std::string_view directory);

        /** @brief Checks that required values and initial display labels are authored. */
        [[nodiscard]] static bool CanConfirmCreateBlackboard(const BlackboardCreationDraft &draft) noexcept;

        /** @brief Reduces a complete Blackboard draft to the normal project command. */
        [[nodiscard]] static std::optional<EditorWorkspaceViewCommandData> ConfirmCreateBlackboard(const BlackboardCreationDraft &draft);

        /** @brief Captures the active asset folder for the independent query creation modal. */
        void OpenCreateQuery(std::string_view directory);

        /** @brief Checks signed draft input and native lattice bounds without allocating. */
        [[nodiscard]] static bool CanConfirmCreateQuery(const EnvironmentQueryCreationDraft &draft) noexcept;

        /** @brief Creates a typed workspace confirmation only for representable, bounded authoring inputs. */
        [[nodiscard]] static std::optional<EditorWorkspaceViewCommandData> ConfirmCreateQuery(const EnvironmentQueryCreationDraft &draft);

        /** @brief Creates an absolute directory-navigation command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Navigate(std::string_view absolutePath);

        /** @brief Creates a history/back/up navigation command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Navigate(EditorWorkspaceViewCommand navigationCommand);

        /** @brief Creates a refresh command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Refresh();

        /** @brief Creates a validated rename command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Rename(std::string_view absolutePath, std::string_view name);

        /** @brief Creates a delete command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Delete(std::string_view absolutePath);

        /** @brief Creates a create-folder command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData CreateFolder(std::string_view absoluteDirectory, std::string_view name);

        /** @brief Creates an asset duplicate command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Duplicate(std::string_view absolutePath);

        /** @brief Creates an asset copy command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Copy(std::string_view absolutePath);

        /** @brief Creates an asset cut command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Cut(std::string_view absolutePath);

        /** @brief Creates an asset paste command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Paste(std::string_view absoluteDirectory);

        /** @brief Creates a clipboard-cancellation command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData CancelClipboard();

        /** @brief Creates an absolute drag/drop transfer command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Transfer(std::string_view absoluteSource,
                                                                     std::string_view absoluteDestinationDirectory,
                                                                     ContentBrowserTransferMode mode);

        /** @brief Creates an asset reimport command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Reimport(std::string_view absolutePath);

        /** @brief Creates a native-file-manager reveal command. */
        [[nodiscard]] static EditorWorkspaceViewCommandData Reveal(std::string_view absolutePath);

        /** @brief Creates an asset-import menu invocation for a directory. */
        [[nodiscard]] static EditorWorkspaceViewCommandData ImportHere(std::string_view absoluteDirectory);

    private:
        [[nodiscard]] static EditorWorkspaceViewCommandData PathCommand(EditorWorkspaceViewCommand command, std::string_view absolutePath);

        AssetBrowserInteractionState m_state;
    };
}  // namespace Horo::Editor
