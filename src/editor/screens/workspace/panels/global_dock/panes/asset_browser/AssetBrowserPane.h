#pragma once

#include "Horo/Editor/Localization/LocalizedGui.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserCards.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"

struct ImVec2;

namespace Horo::Editor {
    struct EditorGuiContext;
    class IEditorGuiRenderer;

    /** @brief Owns and draws the Assets story hosted by the global dock. */
    class AssetBrowserPane : public LocalizedGui {
    public:
        /** @brief Catalog scope for this embedded dock surface. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.content_browser"};

        /** @brief Creates a pane with its required localization scope. */
        AssetBrowserPane() : LocalizedGui(kTextScope) {}

        /** @brief Resolves root presentation without deriving filesystem capabilities from a display label. */
        [[nodiscard]] static constexpr const char *RootLabelKey(const ContentBrowserRootKind kind) noexcept {
            return kind == ContentBrowserRootKind::Assets ? "location.assets" : "location.native_sources";
        }

        /** @brief Binds renderer services required by generated previews. */
        void Attach(IEditorGuiRenderer *guiRenderer) noexcept;

        /** @brief Releases every renderer-owned preview texture. */
        void Detach() noexcept;

        /** @brief Draws the complete Assets tab content. */
        void Draw(const ImVec2 &contentOrigin, float contentWidth, const EditorWorkspaceViewModel &viewModel,
                  EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);

    private:
        AssetBrowserInteractionSession m_interactionSession;
        AssetBrowserCardRenderer m_cardRenderer;
    };
}  // namespace Horo::Editor
