#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSurfaceEventContext.h"
#include "Horo/Editor/WorkspaceDockArea.h"
#include "Horo/Runtime/Input.h"

#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;
using ImU32 = unsigned int;

namespace Horo::Log {
    class IStructuredLogQuery;
}  // namespace Horo::Log

namespace Horo {
    class IBuildOutputQuery;
    class IOperationControl;
    class IOperationQuery;
}  // namespace Horo

namespace Horo::Editor {
    class EditorDataBus;
    class IEditorGuiRenderer;
    class IEditorViewportRenderer;
    struct EditorWorkspaceViewCommandData;
    struct EditorWorkspaceViewModel;

    /** @brief Context injected when a panel is attached to the workspace. */
    struct PanelContext {
        EditorDataBus &dataBus;
        IEditorGuiRenderer *guiRenderer{nullptr};
        IEditorViewportRenderer *viewportRenderer{nullptr};
        Input::InputRouter *inputRouter{nullptr};
        Input::InputContextToken *workspaceInputContext{nullptr};
        const Log::IStructuredLogQuery *logQuery{nullptr};
        const IBuildOutputQuery *buildOutputQuery{nullptr};
        const IOperationQuery *operationQuery{nullptr};
        IOperationControl *operationControl{nullptr};
        EditorSurfaceEventContext *surfaceEvents{nullptr};
    };

    /** @brief Base interface for a modular Workspace Panel (e.g. Hierarchy, Inspector) */
    class IWorkspacePanel {
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

        /** @brief Draws the 24x24 icon for the Activity Bar. */
        virtual void DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, ImU32 color) = 0;

        /** @brief Draws the actual panel content, including its own internal tabs if needed. */
        virtual void DrawPanel(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm,
                               EditorWorkspaceViewCommandData &cmd, const EditorGuiContext &ctx) = 0;
    };
}  // namespace Horo::Editor
