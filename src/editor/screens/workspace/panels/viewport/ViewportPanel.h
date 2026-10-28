#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Extensions/EditorViewportHost.h"
#include "WorldSourceViewportTool.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/viewport/interaction/ViewportInteractionController.h"
#include "navigation/NavigationOverlayLayer.h"

#include <imgui.h>
#include <optional>

namespace Horo::Editor {
    class ViewportPanel final : public IWorkspacePanel {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.viewport"};

        /** @brief Creates a surface with its required localization scope. */
        ViewportPanel() : IWorkspacePanel(kTextScope) {}

        [[nodiscard]] std::string GetId() const override;

        [[nodiscard]] std::string GetDisplayName() const override;
        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override;
        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override;

        void OnAttach(PanelContext &ctx) override;
        void OnDetach() override;
        void OnPresentationHidden(EditorWorkspaceViewCommandData &command) override;

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

        /** @brief Withdraws an unavailable provider transaction through normal preview rollback. */
        [[nodiscard]] bool CancelUnavailableContribution(Extensions::EditorViewportTool tool, bool eligible,
                                                         EditorWorkspaceViewCommandData &command);
        /** @brief Prepares copied geometry and picks only when the host grants idle viewport input. */
        [[nodiscard]] Extensions::EditorViewportPick PrepareContributions(const ViewportSurfaceLayout &layout,
                                                                          Extensions::EditorViewportTool tool, bool editable, bool canPick);
        /** @brief Draws the prepared provider layer after scene affordances and below toolbar controls. */
        void RenderContributions(ImDrawList &drawList, const ViewportSurfaceLayout &layout) const;

        /** @brief Defines the viewport pointer surface only outside toolbar controls or during owned capture. */
        [[nodiscard]] static bool PrepareViewportInput(const ViewportSurfaceLayout &layout, bool inputEligible, bool surfaceInteractive);
        /** @brief Resolves routed workspace eligibility without overriding a higher-priority input context. */
        [[nodiscard]] bool ViewportInputEligible() const noexcept;
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

        static void DrawMissingRendererMessage(float centerX, float originY, float height, const EditorGuiContext &context);

        IEditorViewportRenderer *viewportRenderer_{nullptr};
        Input::InputRouter *inputRouter_{nullptr};
        Input::InputContextToken *workspaceInputContext_{nullptr};
        ViewportInteractionController interaction_;
        WorldSourceViewportTool worldSourceTool_;
        const NavigationViewportAccess *navigationOverlays_{};
        NavigationOverlayLayer navigationLayer_;
        std::filesystem::path navigationProjectRoot_;
        Extensions::EditorViewportHost *contributions_{};
        Extensions::EditorViewportFrame contributionFrame_;
        Extensions::EditorViewportPick activeContribution_;
        bool lightMarkerFailureReported_{false};
        bool assetPlacementPreviewActive_{false};
        bool assetPlacementCancelled_{false};
        std::optional<bool> gridOverride_;
    };
}  // namespace Horo::Editor
