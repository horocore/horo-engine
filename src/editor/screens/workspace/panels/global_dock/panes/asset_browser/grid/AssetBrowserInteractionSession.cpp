#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/grid/AssetBrowserInteractionSession.h"

#include "Horo/AI/DecisionPlanCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool IsAbsolute(const std::string_view path) {
            return !path.empty() && std::filesystem::path{path}.is_absolute();
        }

        [[nodiscard]] bool IsEntryName(const std::string_view name) {
            if (name.empty())
                return false;
            const std::filesystem::path path{name};
            return path == path.filename() && name != "." && name != "..";
        }
    }  // namespace

    /** @copydoc AssetBrowserInteractionSession::State */
    AssetBrowserInteractionState &AssetBrowserInteractionSession::State() noexcept {
        return m_state;
    }

    /** @copydoc AssetBrowserInteractionSession::State */
    const AssetBrowserInteractionState &AssetBrowserInteractionSession::State() const noexcept {
        return m_state;
    }

    /** @copydoc AssetBrowserInteractionSession::ProjectEntries */
    std::vector<std::size_t> AssetBrowserInteractionSession::ProjectEntries(const ContentBrowserDirectory &directory) const {
        return ProjectContentBrowserEntries(directory, ContentBrowserEntryQuery{
                                                           .name = m_state.search.data(),
                                                           .assetType = m_state.assetTypeFilter,
                                                           .sortField = m_state.sortField,
                                                           .sortDirection = m_state.sortDirection,
                                                       });
    }

    /** @copydoc AssetBrowserInteractionSession::Select */
    void AssetBrowserInteractionSession::Select(const std::string_view absolutePath) {
        m_state.selectedAbsolutePath = IsAbsolute(absolutePath) ? std::string{absolutePath} : std::string{};
    }

    /** @copydoc AssetBrowserInteractionSession::OpenRename */
    void AssetBrowserInteractionSession::OpenRename(const ContentBrowserEntry &entry) {
        m_state.popupEntry = entry;
        m_state.renameBuffer.fill('\0');
        const std::size_t copyLength = std::min(entry.displayName.size(), m_state.renameBuffer.size() - 1U);
        std::memcpy(m_state.renameBuffer.data(), entry.displayName.data(), copyLength);
        m_state.openRename = true;
    }

    /** @copydoc AssetBrowserInteractionSession::OpenInfo */
    void AssetBrowserInteractionSession::OpenInfo(const ContentBrowserEntry &entry) {
        m_state.popupEntry = entry;
        m_state.openAssetInfo = true;
    }

    /** @copydoc AssetBrowserInteractionSession::OpenDelete */
    void AssetBrowserInteractionSession::OpenDelete(const ContentBrowserEntry &entry) {
        m_state.popupEntry = entry;
        m_state.openDeleteConfirmation = true;
    }

    /** @copydoc AssetBrowserInteractionSession::OpenCreateFolder */
    void AssetBrowserInteractionSession::OpenCreateFolder() {
        m_state.createFolderBuffer.fill('\0');
        m_state.openCreateFolder = true;
    }

    /** @copydoc AssetBrowserInteractionSession::OpenCreateTaskDecision */
    void AssetBrowserInteractionSession::OpenCreateTaskDecision(std::string_view directory, AI::DecisionPlanKind family,
                                                                std::uint64_t catalogRevision) {
        m_state.createTaskDecision = {.directory = std::string{directory}, .family = family, .catalogRevision = catalogRevision};
        m_state.initialTaskPalette = {};
        if (family == AI::DecisionPlanKind::Utility)
            m_state.createTaskDecision.score.emplace();
        m_state.utilityMinimum = 0;
        m_state.utilityMaximum = 1;
        m_state.openCreateTaskDecision = true;
    }

    /** @copydoc AssetBrowserInteractionSession::CanConfirmTaskDecision */
    bool AssetBrowserInteractionSession::CanConfirmTaskDecision(const TaskDecisionCreationRequest &draft) noexcept {
        if (draft.directory.empty() || !AI::IsValidBlackboardDisplayName(draft.name) || !draft.schema.IsValid() || !draft.task.IsValid() ||
            !draft.provider.IsValid() || draft.version == 0 || draft.catalogRevision == 0)
            return false;
        if (draft.family == AI::DecisionPlanKind::BehaviorTree)
            return !draft.score && draft.version <= std::numeric_limits<std::uint16_t>::max();
        return draft.family == AI::DecisionPlanKind::Utility && draft.score && draft.score->key.IsValid() &&
               std::isfinite(draft.score->maximum - draft.score->minimum) && draft.score->maximum > draft.score->minimum;
    }

    /** @copydoc AssetBrowserInteractionSession::ConfirmTaskDecision */
    std::optional<EditorWorkspaceViewCommandData> AssetBrowserInteractionSession::ConfirmTaskDecision(
        const TaskDecisionCreationRequest &draft) {
        if (!CanConfirmTaskDecision(draft))
            return {};
        return EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::CreateTaskDecision, .newTaskDecision = draft};
    }

    /** @copydoc AssetBrowserInteractionSession::OpenCreateStateMachine */
    void AssetBrowserInteractionSession::OpenCreateStateMachine(std::string_view directory) {
        m_state.createStateMachine = {.directory = std::string{directory}};
        m_state.openCreateStateMachine = true;
    }

    /** @copydoc AssetBrowserInteractionSession::CanConfirmStateMachine */
    bool AssetBrowserInteractionSession::CanConfirmStateMachine(const StateMachineCreationRequest &draft) noexcept {
        return !draft.directory.empty() && AI::IsValidBlackboardDisplayName(draft.name) && draft.schema.IsValid() &&
               (draft.outcome == AI::DecisionStateTerminal::Succeeded || draft.outcome == AI::DecisionStateTerminal::Failed);
    }

    /** @copydoc AssetBrowserInteractionSession::ConfirmStateMachine */
    std::optional<EditorWorkspaceViewCommandData> AssetBrowserInteractionSession::ConfirmStateMachine(
        const StateMachineCreationRequest &draft) {
        if (!CanConfirmStateMachine(draft))
            return {};
        return EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::CreateStateMachine, .newStateMachine = draft};
    }

    /** @copydoc AssetBrowserInteractionSession::OpenCreateBlackboard */
    void AssetBrowserInteractionSession::OpenCreateBlackboard(std::string_view directory) {
        m_state.createBlackboard = {
            .request = {.directory = std::string{directory}, .key = {.presence = AI::BlackboardKeyPresence::Optional}}};
        m_state.openCreateBlackboard = true;
    }

    /** @copydoc AssetBrowserInteractionSession::CanConfirmCreateBlackboard */
    bool AssetBrowserInteractionSession::CanConfirmCreateBlackboard(const BlackboardCreationDraft &draft) noexcept {
        const auto &request = draft.request;
        return !request.directory.empty() && AI::IsValidBlackboardDisplayName(request.name) &&
               AI::IsValidBlackboardDisplayName(request.keyName) &&
               (request.key.presence != AI::BlackboardKeyPresence::Required || request.key.defaultValue.has_value());
    }

    /** @copydoc AssetBrowserInteractionSession::ConfirmCreateBlackboard */
    std::optional<EditorWorkspaceViewCommandData> AssetBrowserInteractionSession::ConfirmCreateBlackboard(
        const BlackboardCreationDraft &draft) {
        if (!CanConfirmCreateBlackboard(draft))
            return {};
        return EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::CreateBlackboardSchema,
                                              .newBlackboard = draft.request};
    }

    /** @copydoc AssetBrowserInteractionSession::OpenCreateQuery */
    void AssetBrowserInteractionSession::OpenCreateQuery(std::string_view directory) {
        m_state.createQuery = {.directory = std::string{directory}};
        m_state.openCreateQuery = true;
    }

    /** @copydoc AssetBrowserInteractionSession::CanConfirmCreateQuery */
    bool AssetBrowserInteractionSession::CanConfirmCreateQuery(const EnvironmentQueryCreationDraft &draft) noexcept {
        if (draft.directory.empty() || draft.name.front() == char{} || draft.maximumResults <= 0 ||
            draft.maximumResults > static_cast<int>(AI::EnvironmentQuerySchemaLimits::MaximumResults))
            return false;
        if (draft.perceivedActors)
            return true;
        if (draft.distance && draft.radiusMillimeters <= 0)
            return false;
        if (draft.pointGenerator != EnvironmentQueryPointGenerator::Lattice)
            return draft.pointGenerator == EnvironmentQueryPointGenerator::Authored ||
                   draft.pointGenerator == EnvironmentQueryPointGenerator::NavigationSurface;
        return draft.stepMillimeters > 0 && draft.outerSteps >= 0 &&
               draft.outerSteps <= static_cast<int>(AI::MaximumQueryLatticeOuterSteps) &&
               (!draft.annulus || (draft.innerSteps >= 0 && draft.innerSteps <= draft.outerSteps));
    }

    /** @copydoc AssetBrowserInteractionSession::ConfirmCreateQuery */
    std::optional<EditorWorkspaceViewCommandData> AssetBrowserInteractionSession::ConfirmCreateQuery(
        const EnvironmentQueryCreationDraft &draft) {
        if (!CanConfirmCreateQuery(draft))
            return {};
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::CreateEnvironmentQuery;
        EnvironmentQueryCreationRequest request{.directory = draft.directory,
                                                .name = {draft.name.begin(), std::ranges::find(draft.name, char{})},
                                                .maximumResults = static_cast<std::uint32_t>(draft.maximumResults)};
        if (draft.perceivedActors) {
            request.perception = draft.currentlySensed ? AI::QueryPerceptionMatch::CurrentlySensed : AI::QueryPerceptionMatch::Remembered;
        } else {
            switch (draft.pointGenerator) {
                case EnvironmentQueryPointGenerator::Authored:
                    request.alternateGenerator = std::vector{Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};
                    break;
                case EnvironmentQueryPointGenerator::NavigationSurface:
                    request.alternateGenerator = AI::QueryNavigationSurfaceSettings{2000, 16};
                    break;
                case EnvironmentQueryPointGenerator::Lattice:
                    request.lattice = {.stepMillimeters = static_cast<std::uint32_t>(draft.stepMillimeters),
                                       .outerSteps = static_cast<std::uint32_t>(draft.outerSteps),
                                       .innerSteps =
                                           draft.annulus ? std::optional{static_cast<std::uint32_t>(draft.innerSteps)} : std::nullopt};
                    break;
            }
            if (draft.distance)
                request.distance = AI::QueryPointDistanceTest{static_cast<std::uint32_t>(draft.radiusMillimeters), draft.preferNearer};
        }
        command.newEnvironmentQuery = std::move(request);
        return command;
    }

    /** @copydoc AssetBrowserInteractionSession::Navigate */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Navigate(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::NavigateContentBrowser, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::Navigate */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Navigate(const EditorWorkspaceViewCommand navigationCommand) {
        using enum EditorWorkspaceViewCommand;
        switch (navigationCommand) {
            case NavigateContentBrowserBack:
            case NavigateContentBrowserForward:
            case NavigateContentBrowserUp:
                return {.command = navigationCommand};
            default:
                return {};
        }
    }

    /** @copydoc AssetBrowserInteractionSession::Refresh */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Refresh() {
        return {.command = EditorWorkspaceViewCommand::RefreshContentBrowser};
    }

    /** @copydoc AssetBrowserInteractionSession::Rename */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Rename(const std::string_view absolutePath,
                                                                          const std::string_view name) {
        if (!IsAbsolute(absolutePath) || !IsEntryName(name))
            return {};
        EditorWorkspaceViewCommandData result;
        result.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry;
        result.stringPayload = std::string{absolutePath};
        result.secondaryStringPayload = std::string{name};
        return result;
    }

    /** @copydoc AssetBrowserInteractionSession::Delete */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Delete(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::DeleteContentBrowserEntry, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::CreateFolder */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::CreateFolder(const std::string_view absoluteDirectory,
                                                                                const std::string_view name) {
        if (!IsAbsolute(absoluteDirectory) || !IsEntryName(name))
            return {};
        EditorWorkspaceViewCommandData result;
        result.command = EditorWorkspaceViewCommand::CreateContentBrowserFolder;
        result.stringPayload = std::string{absoluteDirectory};
        result.secondaryStringPayload = std::string{name};
        return result;
    }

    /** @copydoc AssetBrowserInteractionSession::Duplicate */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Duplicate(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::DuplicateContentBrowserAsset, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::Copy */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Copy(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::CopyContentBrowserAsset, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::Cut */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Cut(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::CutContentBrowserAsset, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::Paste */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Paste(const std::string_view absoluteDirectory) {
        return PathCommand(EditorWorkspaceViewCommand::PasteContentBrowserAsset, absoluteDirectory);
    }

    /** @copydoc AssetBrowserInteractionSession::CancelClipboard */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::CancelClipboard() {
        return {.command = EditorWorkspaceViewCommand::CancelContentBrowserClipboard};
    }

    /** @copydoc AssetBrowserInteractionSession::Transfer */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Transfer(const std::string_view absoluteSource,
                                                                            const std::string_view absoluteDestinationDirectory,
                                                                            const ContentBrowserTransferMode mode) {
        if (!IsAbsolute(absoluteSource) || !IsAbsolute(absoluteDestinationDirectory)) {
            return {};
        }
        EditorWorkspaceViewCommandData result;
        result.command = EditorWorkspaceViewCommand::TransferContentBrowserAsset;
        result.contentBrowserTransfer = ContentBrowserAssetTransferRequest{
            .absoluteSourcePath = std::string{absoluteSource},
            .absoluteDestinationDirectory = std::string{absoluteDestinationDirectory},
            .mode = mode,
        };
        return result;
    }

    /** @copydoc AssetBrowserInteractionSession::Reimport */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Reimport(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::ReimportContentBrowserAsset, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::Reveal */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::Reveal(const std::string_view absolutePath) {
        return PathCommand(EditorWorkspaceViewCommand::RevealContentBrowserEntry, absolutePath);
    }

    /** @copydoc AssetBrowserInteractionSession::ImportHere */
    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::ImportHere(const std::string_view absoluteDirectory) {
        if (!IsAbsolute(absoluteDirectory))
            return {};
        EditorWorkspaceViewCommandData result;
        result.menuInvocation = EditorMenuInvocation{
            .action = EditorMenuAction::ImportAssets,
            .assetDestination = std::filesystem::path{absoluteDirectory}.lexically_normal(),
        };
        return result;
    }

    EditorWorkspaceViewCommandData AssetBrowserInteractionSession::PathCommand(const EditorWorkspaceViewCommand command,
                                                                               const std::string_view absolutePath) {
        if (!IsAbsolute(absolutePath))
            return {};
        EditorWorkspaceViewCommandData result;
        result.command = command;
        result.stringPayload = std::string{absolutePath};
        return result;
    }
}  // namespace Horo::Editor
