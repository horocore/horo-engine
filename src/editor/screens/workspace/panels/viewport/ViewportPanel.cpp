#include "ViewportPanel.h"

#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "ViewportOverlay.h"
#include "editor/screens/workspace/AssetSceneDrop.h"
#include "visualizers/light/LightMarkerLayer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] std::optional<AssetSceneDragPayload> ReadAssetPayload(const ImGuiPayload *payload) {
            if (payload == nullptr || !payload->IsDataType(AssetSceneDragPayloadType) || payload->Data == nullptr ||
                payload->DataSize != sizeof(AssetSceneDragPayload))
                return std::nullopt;
            AssetSceneDragPayload result;
            std::memcpy(&result, payload->Data, sizeof(result));
            if (result.assetId.back() != '\0' || result.assetType.back() != '\0')
                return std::nullopt;
            return result;
        }

        [[nodiscard]] bool ResolveLightMarkerInteraction(const Result<std::optional<SceneObjectId>> &clickedLight, bool &failureReported,
                                                         EditorWorkspaceViewCommandData &command) {
            if (clickedLight.HasError()) {
                if (!failureReported)
                    LOG_ERROR("editor.viewport", "Viewport light marker projection failed: %s", clickedLight.ErrorValue().message.c_str());
                failureReported = true;
                return false;
            }
            failureReported = false;
            if (!clickedLight.Value().has_value())
                return true;
            command.command = EditorWorkspaceViewCommand::SelectObject;
            command.objectPayload = *clickedLight.Value();
            return false;
        }
    }  // namespace

    /** @copydoc ViewportPanel::GetId */
    std::string ViewportPanel::GetId() const {
        return "horo.viewport";
    }

    /** @copydoc ViewportPanel::GetDisplayName */
    std::string ViewportPanel::GetDisplayName() const {
        return "workspace.panel.viewport";
    }

    /** @copydoc ViewportPanel::GetDefaultDockArea */
    WorkspaceDockArea ViewportPanel::GetDefaultDockArea() const {
        return WorkspaceDockArea::Document;
    }

    /** @copydoc ViewportPanel::GetObservedEventTypes */
    std::vector<std::string> ViewportPanel::GetObservedEventTypes() const {
        return {"SceneDocumentChangedEvent", "SelectionChangedEvent"};
    }

    /** @copydoc ViewportPanel::OnAttach */
    void ViewportPanel::OnAttach(PanelContext &context) {
        viewportRenderer_ = context.viewportRenderer;
        if (context.inputRouter != nullptr && context.workspaceInputContext != nullptr)
            interaction_.Attach(*context.inputRouter, *context.workspaceInputContext);
    }

    /** @copydoc ViewportPanel::OnDetach */
    void ViewportPanel::OnDetach() {
        interaction_.Detach();
        viewportRenderer_ = nullptr;
        gridOverride_.reset();
    }

    /** @copydoc ViewportPanel::DrawIcon */
    void ViewportPanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const float originX = position.x + (size.x - 14.0F) * 0.5F;
        const float originY = position.y + (size.y - 14.0F) * 0.5F;
        drawList->AddRect(ImVec2(originX + 2.0F, originY + 3.0F), ImVec2(originX + 12.0F, originY + 11.0F), color, 1.0F, 0, 1.5F);
        drawList->AddCircle(ImVec2(originX + 7.0F, originY + 7.0F), 2.0F, color, 0, 1.5F);
    }

    /** @copydoc ViewportPanel::DrawPanel */
    void ViewportPanel::DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::BeginChild("##Content", ImVec2(size.x, size.y), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

        ImDrawList &drawList = *ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = size.x;
        const float height = size.y;
        const float centerX = origin.x + width * 0.5F;
        const float horizon = origin.y + height * 0.38F;
        const float ground = origin.y + height;
        const ViewportSurfaceLayout surfaceLayout{
            .origin = origin,
            .width = width,
            .height = height,
            .centerX = centerX,
            .horizon = horizon,
            .ground = ground,
        };

        RequestViewportExtent(width, height);
        ConfigureRenderer(viewModel, context);
        const EditorViewportTextureView textureView =
            viewportRenderer_ != nullptr ? viewportRenderer_->TextureView() : EditorViewportTextureView{};
        const bool hasRenderedViewport = viewportRenderer_ != nullptr && viewportRenderer_->IsReady() && textureView.IsValid();
        DrawViewportSurface(drawList, surfaceLayout, textureView, hasRenderedViewport);

        if (hasRenderedViewport && width > 0.0F && height > 0.0F)
            DrawInteractiveViewport(drawList, surfaceLayout, viewModel, command, context, viewportRenderer_->ClipDepthRange());

        const ViewportOverlayAction overlay =
            DrawViewportOverlay(origin, {width, height},
                                ViewportOverlayState{.camera = viewModel.viewportCamera,
                                                     .tool = viewModel.activeTransformTool,
                                                     .gridVisible = gridOverride_.value_or(context.settings.settings.gridOverlay),
                                                     .canFocusSelection = viewModel.primarySelectionWorldBounds.has_value(),
                                                     .objectCount = viewModel.objects.size()},
                                context.theme.fonts, context.localization);
        if (overlay.projection) {
            command.command = EditorWorkspaceViewCommand::ChangeViewportProjection;
            command.viewportProjectionPayload = *overlay.projection;
        } else if (overlay.axisView) {
            command.command = EditorWorkspaceViewCommand::AlignViewportToAxis;
            command.viewportAxisPayload = *overlay.axisView;
        } else if (overlay.tool) {
            command.command = EditorWorkspaceViewCommand::ChangeTransformTool;
            command.transformToolPayload = *overlay.tool;
        } else if (overlay.focusSelection && viewModel.primarySelectionWorldBounds && height > 0.0F) {
            command.command = EditorWorkspaceViewCommand::FocusViewportSelection;
            command.floatPayload = width / height;
        }
        if (overlay.toggleGrid)
            gridOverride_ = !gridOverride_.value_or(context.settings.settings.gridOverlay);
        if (!hasRenderedViewport)
            DrawMissingRendererMessage(centerX, origin.y, height, context);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        static_cast<void>(position);
    }

    void ViewportPanel::ConfigureRenderer(const EditorWorkspaceViewModel &viewModel, const EditorGuiContext &context) const {
        if (viewportRenderer_ == nullptr)
            return;
        viewportRenderer_->RequestGrid(EditorViewportGridOptions{
            .visible = gridOverride_.value_or(context.settings.settings.gridOverlay),
        });
        EditorViewportLightVisualizerOptions lightVisualizer;
        if (viewModel.primarySelection.has_value()) {
            const auto selected =
                std::ranges::find(viewModel.viewportLights, *viewModel.primarySelection, &ViewportLightPresentation::object);
            if (selected != viewModel.viewportLights.end())
                lightVisualizer.selectedLight = selected->light;
        }
        viewportRenderer_->RequestLightVisualizer(lightVisualizer);
    }

    void ViewportPanel::CancelAssetPlacementPreview(EditorWorkspaceViewCommandData &command) {
        if (!assetPlacementPreviewActive_)
            return;
        command = {};
        command.command = EditorWorkspaceViewCommand::CancelAssetPlacementPreview;
        assetPlacementPreviewActive_ = false;
    }

    void ViewportPanel::DrawAssetPlacementHint(ImDrawList &drawList, const ViewportSurfaceLayout &layout, const ImVec2 pointer,
                                               const EditorGuiContext &context) {
        const std::string &hint = context.localization.Get("editor", "workspace.viewport.asset_drop_hint");
        const ImVec2 hintSize = ImGui::CalcTextSize(hint.c_str());
        const ImVec2 hintMin{std::clamp(pointer.x + 14.0F, layout.origin.x + 8.0F, layout.origin.x + layout.width - hintSize.x - 20.0F),
                             std::clamp(pointer.y + 18.0F, layout.origin.y + 8.0F, layout.origin.y + layout.height - hintSize.y - 16.0F)};
        drawList.AddRectFilled(hintMin, {hintMin.x + hintSize.x + 12.0F, hintMin.y + hintSize.y + 8.0F}, Theme::U32(Theme::Bg0()),
                               Theme::Layout::Radius);
        drawList.AddText({hintMin.x + 6.0F, hintMin.y + 4.0F}, Theme::U32(Theme::Text()), hint.c_str());
    }

    bool ViewportPanel::HandleAcceptedAssetDrop(const ImGuiPayload *accepted, ImDrawList &drawList, const ViewportSurfaceLayout &layout,
                                                const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                                const EditorGuiContext &context, const Math::ClipDepthRange depthRange) {
        const std::optional<AssetSceneDragPayload> payload = ReadAssetPayload(accepted);
        if (!payload.has_value()) {
            CancelAssetPlacementPreview(command);
            return false;
        }

        const AssetSceneDropPolicyResult policy = EvaluateAssetSceneDrop(*payload);
        drawList.AddRect(layout.origin, {layout.origin.x + layout.width, layout.origin.y + layout.height},
                         Theme::U32(policy.canInstantiate ? Theme::Accent() : Theme::Err()), 3.0F, 0, 2.0F);
        if (!policy.canInstantiate) {
            CancelAssetPlacementPreview(command);
            return true;
        }

        const ImVec2 pointer = ImGui::GetMousePos();
        const AssetSceneDropRequest request{
            .assetId = payload->assetId.data(),
            .assetType = payload->assetType.data(),
            .absoluteAssetPath = payload->absolutePath.data(),
            .parent = std::nullopt,
            .target = AssetSceneDropTarget::Viewport,
            .normalizedX = std::clamp((pointer.x - layout.origin.x) / layout.width, 0.0F, 1.0F),
            .normalizedY = std::clamp((pointer.y - layout.origin.y) / layout.height, 0.0F, 1.0F),
            .aspect = layout.width / layout.height,
            .depthRange = depthRange,
            .documentRevision = viewModel.documentRevision,
        };
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            CancelAssetPlacementPreview(command);
            assetPlacementCancelled_ = true;
        } else if (!assetPlacementCancelled_) {
            command = {};
            command.command =
                accepted->IsDelivery() ? EditorWorkspaceViewCommand::InstantiateAsset : EditorWorkspaceViewCommand::PreviewAssetPlacement;
            command.assetSceneDrop = request;
            assetPlacementPreviewActive_ = !accepted->IsDelivery();
            DrawAssetPlacementHint(drawList, layout, pointer, context);
        }
        return true;
    }

    bool ViewportPanel::AcceptViewportAssetDrop(ImDrawList &drawList, const ViewportSurfaceLayout &layout,
                                                const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                                const EditorGuiContext &context, const Math::ClipDepthRange depthRange) {
        if (ImGui::GetDragDropPayload() == nullptr)
            assetPlacementCancelled_ = false;
        if (!ImGui::BeginDragDropTarget()) {
            CancelAssetPlacementPreview(command);
            return false;
        }
        const ImGuiPayload *accepted =
            ImGui::AcceptDragDropPayload(AssetSceneDragPayloadType,
                                         ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        const bool active = HandleAcceptedAssetDrop(accepted, drawList, layout, viewModel, command, context, depthRange);
        ImGui::EndDragDropTarget();
        return active;
    }

    void ViewportPanel::DrawInteractiveViewport(ImDrawList &drawList, const ViewportSurfaceLayout &layout,
                                                const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                                const EditorGuiContext &context, const Math::ClipDepthRange depthRange) {
        const ImVec2 pointer = ImGui::GetMousePos();
        const bool pointerOverControls = pointer.y >= layout.origin.y && pointer.y <= layout.origin.y + 100.0F;
        const bool surfaceInteractive = !pointerOverControls || interaction_.IsActive();
        bool surfaceHovered = false;
        if (surfaceInteractive) {
            ImGui::SetCursorScreenPos(layout.origin);
            ImGui::InvisibleButton("##ViewportSurface", {layout.width, layout.height},
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                       ImGuiButtonFlags_MouseButtonMiddle);
            surfaceHovered = ImGui::IsItemHovered();
        }
        if (!surfaceInteractive)
            CancelAssetPlacementPreview(command);
        const bool assetDragActive =
            surfaceInteractive && AcceptViewportAssetDrop(drawList, layout, viewModel, command, context, depthRange);
        const Result<std::optional<SceneObjectId>> clickedLight =
            DrawViewportLightMarkers({.drawList = drawList,
                                      .origin = layout.origin,
                                      .width = layout.width,
                                      .height = layout.height,
                                      .camera = viewModel.viewportCamera,
                                      .depthRange = depthRange,
                                      .acceptInput = surfaceHovered && !interaction_.IsActive() && !assetDragActive},
                                     viewModel.viewportLights, viewModel.primarySelection);
        if (ResolveLightMarkerInteraction(clickedLight, lightMarkerFailureReported_, command) && surfaceInteractive && !assetDragActive) {
            interaction_.Draw({.drawList = drawList,
                               .origin = layout.origin,
                               .width = layout.width,
                               .height = layout.height,
                               .hovered = surfaceHovered,
                               .viewModel = viewModel,
                               .command = command,
                               .gui = context,
                               .deltaSeconds = ImGui::GetIO().DeltaTime,
                               .depthRange = depthRange});
        }
    }

    void ViewportPanel::RequestViewportExtent(const float width, const float height) const noexcept {
        if (viewportRenderer_ == nullptr)
            return;
        const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        const bool renderable = width > 0.0F && height > 0.0F && framebufferScale.x > 0.0F && framebufferScale.y > 0.0F;
        viewportRenderer_->RequestExtent(
            renderable
                ? EditorViewportExtent{
                      .width = static_cast<std::uint32_t>(
                          std::max(1.0F, width * framebufferScale.x)),
                      .height = static_cast<std::uint32_t>(
                          std::max(1.0F, height * framebufferScale.y)),
                  }
                : EditorViewportExtent{});
    }

    void ViewportPanel::DrawViewportSurface(ImDrawList &drawList, const ViewportSurfaceLayout &layout,
                                            const EditorViewportTextureView &textureView, const bool hasRenderedViewport) {
        const auto &[origin, width, height, centerX, horizon, ground] = layout;

        if (hasRenderedViewport) {
            const auto texture = static_cast<ImTextureID>(textureView.textureId);
            drawList.AddImage(texture, origin, ImVec2(origin.x + width, origin.y + height), ImVec2(textureView.u0, textureView.v0),
                              ImVec2(textureView.u1, textureView.v1));
            return;
        }

        drawList.AddRectFilledMultiColor(origin, ImVec2(origin.x + width, origin.y + height),
                                         ImGui::GetColorU32(ImVec4(0.05F, 0.06F, 0.09F, 1.0F)),
                                         ImGui::GetColorU32(ImVec4(0.05F, 0.06F, 0.09F, 1.0F)),
                                         ImGui::GetColorU32(ImVec4(0.09F, 0.11F, 0.15F, 1.0F)),
                                         ImGui::GetColorU32(ImVec4(0.09F, 0.11F, 0.15F, 1.0F)));

        const ImU32 gridColor = ImGui::GetColorU32(ImVec4(0.16F, 0.20F, 0.27F, 1.0F));
        constexpr int lineCount = 14;
        for (int line = 0; line <= lineCount; ++line) {
            const float ratio = static_cast<float>(line) / lineCount;
            const float xOffset = (ratio - 0.5F) * width;
            drawList.AddLine(ImVec2(centerX + xOffset, ground), ImVec2(centerX, horizon), gridColor, 0.7F);

            const float yPosition = ground - ratio * (ground - horizon);
            const float halfWidth = width * (1.0F - ratio * 0.90F) * 0.5F;
            drawList.AddLine(ImVec2(centerX - halfWidth, yPosition), ImVec2(centerX + halfWidth, yPosition), gridColor, 0.7F);
        }
        drawList.AddRectFilledMultiColor(ImVec2(origin.x, horizon - 12.0F), ImVec2(origin.x + width, horizon + 22.0F),
                                         ImGui::GetColorU32(ImVec4(0.01F, 0.22F, 0.44F, 0.0F)),
                                         ImGui::GetColorU32(ImVec4(0.01F, 0.22F, 0.44F, 0.0F)),
                                         ImGui::GetColorU32(ImVec4(0.03F, 0.38F, 0.60F, 0.35F)),
                                         ImGui::GetColorU32(ImVec4(0.03F, 0.38F, 0.60F, 0.35F)));
    }

    void ViewportPanel::DrawMissingRendererMessage(const float centerX, const float originY, const float height,
                                                   const EditorGuiContext &context) {
        const std::string &message = context.localization.Get("editor", "workspace.viewport.renderer_missing");
        const float messageWidth = ImGui::CalcTextSize(message.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(centerX - messageWidth * 0.5F, originY + height * 0.52F));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.28F, 0.32F, 0.40F, 1.0F));
        ImGui::TextUnformatted(message.c_str());
        ImGui::PopStyleColor();
    }
}  // namespace Horo::Editor
