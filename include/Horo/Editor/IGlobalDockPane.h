#pragma once

/**
 * @file IGlobalDockPane.h
 * @brief Extensible pane contract for the editor global bottom dock.
 */

#include "Horo/Editor/IWorkspacePanel.h"

#include <functional>
#include <imgui.h>
#include <memory>
#include <string_view>

namespace Horo::Editor {
    struct EditorGuiContext;
    struct EditorWorkspaceViewCommandData;
    struct EditorWorkspaceViewModel;

    /** @brief Frame-local, non-owning inputs supplied to one global-dock pane. */
    struct GlobalDockPaneDrawContext {
        ImVec2 contentOrigin;                      /**< Upper-left pane content origin in screen coordinates. */
        float contentWidth;                        /**< Width available to the pane. */
        const EditorWorkspaceViewModel &viewModel; /**< Immutable workspace projection for this frame. */
        EditorWorkspaceViewCommandData &command;   /**< Command output owned by the workspace view. */
        const EditorGuiContext &gui;               /**< Theme, localization, settings, and event capabilities. */
    };

    /** @brief Owned extension point implemented by built-in and module-provided global-dock panes. */
    class IGlobalDockPane : public LocalizedGui {
    protected:
        using LocalizedGui::LocalizedGui;

    public:
        virtual ~IGlobalDockPane() = default;

        /** @brief Returns the stable, globally unique pane identity. */
        [[nodiscard]] virtual std::string_view Id() const noexcept = 0;

        /** @brief Returns the editor-localization key used by the tab strip. */
        [[nodiscard]] virtual std::string_view LabelKey() const noexcept = 0;

        /** @brief Binds optional editor services before the pane receives frames. */
        virtual void Attach(PanelContext &context) {
            static_cast<void>(context);
        }

        /** @brief Releases service references before the workspace is destroyed. */
        virtual void Detach() {}

        /** @brief Draws one frame inside the pane-owned bottom-dock region. */
        virtual void Draw(const GlobalDockPaneDrawContext &context) = 0;
    };

    /** @brief Composition-time factory for one module-provided bottom-dock pane. */
    using GlobalDockPaneFactory = std::function<std::unique_ptr<IGlobalDockPane>()>;
}  // namespace Horo::Editor
