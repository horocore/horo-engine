#pragma once

#include "Horo/Editor/AgentEditorDiagnosticsSnapshot.h"
#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSurfaceEventContext.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "Horo/Editor/WorkspaceDockArea.h"
#include "Horo/Runtime/Input.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawList;
struct ImVec2;
using ImU32 = unsigned int;

namespace Horo::Log {
    class IStructuredLogQuery;
}  // namespace Horo::Log

namespace Horo::Application {
    class GameplayBuildService;
    class INetworkDebuggerQuery;
    class INetworkDebuggerControl;
}  // namespace Horo::Application

namespace Horo::Platform {
    class IProjectReadFiles;
}

namespace Horo {
    class CancellationToken;
    class IBuildOutputQuery;
    class IOperationControl;
    class IOperationQuery;
}  // namespace Horo

namespace Horo::Extensions {
    class EditorInspectorHost;
    class EditorViewportHost;
    struct EditorThemeFrame;
}  // namespace Horo::Extensions

namespace Horo::PlatformServices {
    class IPlatformDiagnostics;
}

namespace Horo::Editor {
    struct NavigationViewportAccess;
    class EditorDataBus;
    class XRSimulatorModel;
    class IXRSimulatorArtifacts;
    class IEditorGuiRenderer;
    class IEditorViewportRenderer;
    class SourceDocumentSnapshot;
    struct SourceContextRange;
    struct EditorWorkspaceViewCommandData;
    struct EditorWorkspaceViewModel;
    struct WorkspaceMcpAuditReference;

    namespace Proposals {
        class ChangeSet;
        class ProjectPolicy;
        class ReviewDecisions;
        struct SourceTransactionStatus;
        struct WorkspaceEditPlan;
    }  // namespace Proposals

    /** @brief Explicit validation action selected by the user in a source review. */
    enum class SourceReviewValidationAction : std::uint8_t {
        Build,
        Test
    };

    /** @brief Context injected when a panel is attached to the workspace. */
    struct PanelContext {
        EditorDataBus &dataBus;
        IEditorGuiRenderer *guiRenderer{nullptr};
        IEditorViewportRenderer *viewportRenderer{nullptr};
        Input::InputRouter *inputRouter{nullptr};
        Input::InputContextToken *workspaceInputContext{nullptr};
        const Log::IStructuredLogQuery *logQuery{nullptr};
        const IBuildOutputQuery *buildOutputQuery{nullptr};
        const Application::GameplayBuildService *gameplayBuilds{nullptr};
        std::string_view projectRoot;
        const IOperationQuery *operationQuery{nullptr};
        IOperationControl *operationControl{nullptr};
        EditorSurfaceEventContext *surfaceEvents{nullptr};
        const Application::INetworkDebuggerQuery *networkDebuggerQuery{};
        Application::INetworkDebuggerControl *networkDebuggerControl{};
        Extensions::EditorInspectorHost *extensionInspectors{}; /**< Explicit owner-lane host, outliving panel attachment. */
        const NavigationViewportAccess *navigationOverlays{};   /**< Explicit owner-lane saved/cooked geometry query. */
        Extensions::EditorViewportHost *extensionViewport{};    /**< Explicit owner of viewport publication and picking. */
        const Extensions::EditorThemeFrame *extensionTheme{};   /**< Current host theme capture; no panel-owned authority. */
        std::weak_ptr<IXRSimulatorArtifacts>
            xrSimulatorArtifacts{};      /**< Same admitted project source/publication port, revoked before retirement. */
        XRSimulatorModel *xrSimulator{}; /**< Explicit application model; invalidated when panel detaches. */
        PlatformServices::IPlatformDiagnostics *platformDiagnostics{};  /**< Host-owned owner-lane diagnostics capability. */
        std::function<std::optional<std::size_t>()> sceneObjectCount{}; /**< Narrow owner-lane authoring observation. */
        std::string_view projectIdentity{};                             /**< Validated project identity borrowed during attachment. */
        const std::filesystem::path *projectFileRoot{};                 /**< Exact native root borrowed during attachment. */
        /** @brief Copies the already-open active Source document on the owner lane, leaving output unchanged on failure. */
        std::function<bool(SourceDocumentSnapshot &)> activeSourceSnapshot{};
        /** @brief Copies the current nonempty source selection and its exact snapshot on the owner lane. */
        std::function<bool(SourceDocumentSnapshot &, SourceContextRange &)> activeSourceRange{};
        /** @brief Borrows the current list of open Source snapshots for a synchronous owner-lane read.
         * @details Copy chosen snapshots before returning to the workspace. The borrow expires on the next model update.
         */
        std::function<std::span<const SourceDocumentSnapshot>()> openSourceSnapshots{};
        /** @brief Captures bounded selected Scene object IDs on the workspace owner lane. */
        std::function<std::optional<AgentSceneSelectionSnapshot>()> captureSceneSelection{};
        /** @brief Revalidates captured scene references on the workspace owner lane for a read-only review. */
        std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> reviewAgentSceneReferences{};
        /** @brief Optional rooted read authority for explicit saved-file review, bound to generation one. */
        std::shared_ptr<const Platform::IProjectReadFiles> agentProjectFiles{};
        std::string_view agentProjectReadIdentity{}; /**< Exact workspace reader identity, borrowed during attachment. */
        /** @brief Captures findings from the active editor document on the workspace owner lane. */
        std::function<std::optional<AgentEditorDiagnosticsSnapshot>()> captureEditorDiagnostics{};
        /** @brief Stages an exact planned operation for presentation after the workspace draw. */
        std::function<bool(const Proposals::ChangeSet &, const Proposals::WorkspaceEditPlan &, std::size_t,
                           EditorWorkspaceViewCommandData &)>
            requestSourceReview{};
        /** @brief Stages the user's explicit request to apply the accepted text after drawing. */
        std::function<bool(const Proposals::ReviewDecisions &, const Proposals::ProjectPolicy &,
                           const std::shared_ptr<const Proposals::WorkspaceEditPlan> &, const CancellationToken &,
                           EditorWorkspaceViewCommandData &)>
            requestSourceReviewApply{};
        /** @brief Stages group undo for the last applied source review. */
        std::function<bool(const CancellationToken &, EditorWorkspaceViewCommandData &)> requestSourceReviewUndo{};
        /** @brief Reads the current workspace's source transaction outcome and undo availability. */
        std::function<Proposals::SourceTransactionStatus()> sourceReviewTransaction{};
        /** @brief Stages one explicit validation action for the currently shown review. */
        std::function<void(SourceReviewValidationAction, const Proposals::SourceTransactionStatus &, const CancellationToken &,
                           EditorWorkspaceViewCommandData &)>
            requestSourceReviewValidation{};
        /** @brief Stages presentation of one exact redacted MCP operation in the workspace dock. */
        std::function<void(const WorkspaceMcpAuditReference &, EditorWorkspaceViewCommandData &)> requestMcpAudit{};
        /** @brief Copies bounded numeric camera state and selection on explicit user request. */
        std::function<std::optional<AgentSceneSelectionSnapshot>(AgentViewportComponents)> captureAgentViewport{};
        /** @brief Opens chat and stages the exact viewport selection for explicit review. */
        std::function<void(EditorWorkspaceViewCommandData &)> offerAgentViewportReview{};
        /** @brief Transfers one pending viewport review to the attached chat. */
        std::function<std::optional<AgentSceneSelectionSnapshot>()> takeAgentViewportReview{};
    };

    /** @brief Base interface for a modular Workspace Panel (e.g. Hierarchy, Inspector) */
    class IWorkspacePanel : public LocalizedGui {
    protected:
        using LocalizedGui::LocalizedGui;

    public:
        virtual ~IWorkspacePanel() = default;

        /** @brief Unique stable identifier for this panel (e.g. "horo.hierarchy") */
        [[nodiscard]] virtual std::string GetId() const = 0;

        /** @brief i18n localization key for the panel's display name (e.g. "horo.panel.hierarchy.title") */
        [[nodiscard]] virtual std::string GetDisplayName() const = 0;

        /** @brief The dock area where this panel should open by default. */
        [[nodiscard]] virtual WorkspaceDockArea GetDefaultDockArea() const = 0;

        /** @brief Returns a list of event types this panel wants to observe via the DataBus. */
        [[nodiscard]] virtual std::vector<std::string> GetObservedEventTypes() const = 0;

        /** @brief Called once when the registry mounts the panel. */
        virtual void OnAttach(PanelContext &ctx) = 0;

        /** @brief Called once before the registry unmounts the panel. */
        virtual void OnDetach() = 0;

        /** @brief Withdraws transient presentation when another document owns this panel's region.
         * @param command Current frame command; an existing document action retains priority.
         * @details Default preserves panels without transient input. Owners must release only their own capture and cancel drafts.
         */
        virtual void OnPresentationHidden(EditorWorkspaceViewCommandData &command) {
            static_cast<void>(command);
        }

        /** @brief Draws the 24x24 icon for the Activity Bar. */
        virtual void DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, ImU32 color) = 0;

        /** @brief Draws the actual panel content, including its own internal tabs if needed. */
        virtual void DrawPanel(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm,
                               EditorWorkspaceViewCommandData &cmd, const EditorGuiContext &ctx) = 0;
    };
}  // namespace Horo::Editor
