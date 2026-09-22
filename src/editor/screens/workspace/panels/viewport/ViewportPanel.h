#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/viewport/interaction/ViewportInteractionController.h"

#include <imgui.h>

namespace Horo::Editor {
    class ViewportPanel final : public IWorkspacePanel {
    public:
        [[nodiscard]] std::string GetId() const override;

        [[nodiscard]] std::string GetDisplayName() const override;
        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override;
        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override;

        void OnAttach(PanelContext &ctx) override;
        void OnDetach() override;

        void DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, ImU32 color) override;

        void DrawPanel(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &cmd,
                       const EditorGuiContext &ctx) override;

    private:
        struct ViewportSurfaceLayout {
            ImVec2 origin{};
            float width{0.0F};
            float height{0.0F};
            float centerX{0.0F};
            float horizon{0.0F};
            float ground{0.0F};
        };

        void RequestViewportExtent(float width, float height) const noexcept;
        void ConfigureRenderer(const EditorWorkspaceViewModel &viewModel, const EditorGuiContext &context) const;
        void DrawInteractiveViewport(ImDrawList &drawList, const ViewportSurfaceLayout &layout, const EditorWorkspaceViewModel &viewModel,
                                     EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                                     Math::ClipDepthRange depthRange);
        bool AcceptViewportAssetDrop(ImDrawList &drawList, const ViewportSurfaceLayout &layout, const EditorWorkspaceViewModel &viewModel,
                                     EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                                     Math::ClipDepthRange depthRange);
        bool HandleAcceptedAssetDrop(const ImGuiPayload *accepted, ImDrawList &drawList, const ViewportSurfaceLayout &layout,
                                     const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                     const EditorGuiContext &context, Math::ClipDepthRange depthRange);
        void CancelAssetPlacementPreview(EditorWorkspaceViewCommandData &command);
        static void DrawAssetPlacementHint(ImDrawList &drawList, const ViewportSurfaceLayout &layout, ImVec2 pointer,
                                           const EditorGuiContext &context);
        static void DrawViewportSurface(ImDrawList &drawList, const ViewportSurfaceLayout &layout,
                                        const EditorViewportTextureView &textureView, bool hasRenderedViewport);

        static void DrawProjectionControl(const ImVec2 &origin, const EditorWorkspaceViewModel &viewModel,
                                          EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        static void DrawObjectCount(const ImVec2 &origin, const EditorWorkspaceViewModel &viewModel, const EditorGuiContext &context);
        static void DrawMissingRendererMessage(float centerX, float originY, float height, const EditorGuiContext &context);

        IEditorViewportRenderer *viewportRenderer_{nullptr};
        ViewportInteractionController interaction_;
        bool lightMarkerFailureReported_{false};
        bool assetPlacementPreviewActive_{false};
        bool assetPlacementCancelled_{false};
    };
}  // namespace Horo::Editor
