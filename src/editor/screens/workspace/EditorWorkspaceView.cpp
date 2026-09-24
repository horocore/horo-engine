#include "EditorWorkspaceView.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/menu/EditorMenuPlatform.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace Horo::Editor {
    constexpr float kMenuBarH = 32.0F;
    constexpr float kRecoveryBarH = 40.0F;
    constexpr float kActivityBarW = 42.0F;
    constexpr float kMinimumDocumentW = 120.0F;
    constexpr float kMinimumMainH = 100.0F;

    namespace {
        struct AllocationTarget {
            const char *windowId;
            WorkspaceDockArea area;
            ActivityBarSlot appendSlot;
            std::optional<BottomDockSlot> bottomSlot;
            std::optional<SideDockSlot> sideSlot;
            ImVec2 hitPos;
            ImVec2 hitSize;
            ImVec2 previewPos;
            ImVec2 previewSize;
            bool preserveActivitySlotWithinArea = false;
        };

        [[nodiscard]] bool SlotBelongsToArea(const ActivityBarSlot &slot, const WorkspaceDockArea area) {
            using enum WorkspaceDockArea;
            switch (area) {
                case Left:
                    return slot.rail == ActivityBarRail::Left && slot.groupIndex < 2;
                case Right:
                    return slot.rail == ActivityBarRail::Right && slot.groupIndex < 2;
                case Bottom:
                    return (slot.rail == ActivityBarRail::Left || slot.rail == ActivityBarRail::Right) && slot.groupIndex == 2;
                case Document:
                    return slot.rail == ActivityBarRail::DocumentTop && slot.groupIndex == 0;
            }
            return false;
        }

        [[nodiscard]] bool IsActivityItemActive(const std::string_view panelId, const EditorWorkspaceViewModel &viewModel) {
            return panelId == viewModel.activeLeftPanelId || panelId == viewModel.activeRightPanelId ||
                   panelId == viewModel.activeLeftTopPanelId || panelId == viewModel.activeLeftBottomPanelId ||
                   panelId == viewModel.activeRightTopPanelId || panelId == viewModel.activeRightBottomPanelId ||
                   panelId == viewModel.activeBottomLeftPanelId || panelId == viewModel.activeBottomRightPanelId ||
                   panelId == viewModel.activeBottomPanelId || panelId == viewModel.activeDocumentPanelId;
        }

        [[nodiscard]] bool IsActiveInBottomSplit(const std::string_view panelId, const EditorWorkspaceViewModel &viewModel) {
            return viewModel.bottomDockMode == BottomDockMode::Split &&
                   (panelId == viewModel.activeBottomLeftPanelId || panelId == viewModel.activeBottomRightPanelId);
        }

        [[nodiscard]] bool IsActiveInSideSplit(const std::string_view panelId, const EditorWorkspaceViewModel &viewModel) {
            return (viewModel.leftDockMode == SideDockMode::Split &&
                    (panelId == viewModel.activeLeftTopPanelId || panelId == viewModel.activeLeftBottomPanelId)) ||
                   (viewModel.rightDockMode == SideDockMode::Split &&
                    (panelId == viewModel.activeRightTopPanelId || panelId == viewModel.activeRightBottomPanelId));
        }

        [[nodiscard]] int ActivityBarAreaIndex(const WorkspaceDockArea area) {
            using enum WorkspaceDockArea;
            switch (area) {
                case Left:
                    return 0;
                case Right:
                    return 1;
                case Bottom:
                    return 2;
                case Document:
                    return 3;
            }
            return 3;
        }

        /** @brief Begins an opaque border-backed workspace surface with shared window styling. */
        void BeginWorkspaceSurface(const char *id, const ImVec2 position, const ImVec2 size, const ImVec2 padding,
                                   const ImGuiWindowFlags flags) {
            ImGui::SetNextWindowPos(position);
            ImGui::SetNextWindowSize(size);
            ImGui::SetNextWindowBgAlpha(1.0F);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg1());
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
            ImGui::Begin(id, nullptr, flags);
        }

        void DrawAllocationTarget(const AllocationTarget &target, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &outCommand, const bool panelDragEligible) {
            if (target.hitSize.x <= 0.0F || target.hitSize.y <= 0.0F) {
                return;
            }
            ImGui::SetNextWindowPos(target.hitPos);
            ImGui::SetNextWindowSize(target.hitSize);
            ImGui::SetNextWindowBgAlpha(0.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
            ImGui::Begin(target.windowId, nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavInputs |
                             ImGuiWindowFlags_NoNavFocus);
            ImGui::SetCursorPos(ImVec2(0.0F, 0.0F));
            ImGui::InvisibleButton("##ActivityPanelAllocationTarget", target.hitSize);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                ImDrawList *drawList = ImGui::GetWindowDrawList();
                const ImVec2 previewMax(target.previewPos.x + target.previewSize.x, target.previewPos.y + target.previewSize.y);
                drawList->PushClipRectFullScreen();
                drawList->AddRectFilled(target.previewPos, previewMax, Theme::U32(Theme::AccentSoft()), 4.0F);
                drawList->AddRect(ImVec2(target.previewPos.x + 0.5F, target.previewPos.y + 0.5F),
                                  ImVec2(previewMax.x - 0.5F, previewMax.y - 0.5F), Theme::U32(Theme::Accent()), 4.0F, 0, 1.0F);
                drawList->PopClipRect();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *payload =
                        ImGui::AcceptDragDropPayload("HORO_ACTIVITY_BAR_PANEL", ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                    payload != nullptr && panelDragEligible) {
                    const std::string_view panelId(static_cast<const char *>(payload->Data));
                    outCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
                    outCommand.targetIndex = static_cast<int>(target.area);
                    outCommand.stringPayload = panelId;
                    if (const std::optional<ActivityBarSlot> currentSlot = viewModel.activityBarLayout.FindSlot(panelId);
                        !target.preserveActivitySlotWithinArea || !currentSlot.has_value() ||
                        !SlotBelongsToArea(*currentSlot, target.area)) {
                        outCommand.activityBarSlot = target.appendSlot;
                    }
                    outCommand.bottomDockSlot = target.bottomSlot;
                    outCommand.sideDockSlot = target.sideSlot;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
        }

        [[nodiscard]] WorkspaceSplitterInteractionResult UpdateSplitters(const EditorWorkspaceView::WorkspaceLayoutGeometry &geo,
                                                                         WorkspaceSplitterInteraction &splitterInteraction,
                                                                         Input::InputRouter &inputRouter,
                                                                         const Input::InputContextToken &workspaceInputContext) {
            std::array<WorkspaceSplitterRegion, 3> splitterRegions{};
            std::size_t splitterRegionCount = 0;
            const auto addSplitterRegion = [&splitterRegions, &splitterRegionCount](const WorkspaceSplitterId id,
                                                                                    const WorkspaceSplitterAxis axis, const ImVec2 &pos,
                                                                                    const ImVec2 &size) {
                splitterRegions[splitterRegionCount++] = WorkspaceSplitterRegion{.id = id,
                                                                                 .axis = axis,
                                                                                 .minX = pos.x,
                                                                                 .minY = pos.y,
                                                                                 .maxX = pos.x + size.x,
                                                                                 .maxY = pos.y + size.y};
            };
            if (geo.hierarchyW > 0.0F) {
                addSplitterRegion(WorkspaceSplitterId::Left, WorkspaceSplitterAxis::Horizontal,
                                  ImVec2(geo.leftActivityW + geo.hierarchyW - 4.0F, geo.curY), ImVec2(8.0F, geo.mainH));
            }
            if (geo.inspectorW > 0.0F) {
                addSplitterRegion(WorkspaceSplitterId::Right, WorkspaceSplitterAxis::Horizontal,
                                  ImVec2(geo.display.x - geo.rightActivityW - geo.inspectorW - 4.0F, geo.curY), ImVec2(8.0F, geo.mainH));
            }
            if (geo.contentH > 0.0F) {
                addSplitterRegion(WorkspaceSplitterId::Bottom, WorkspaceSplitterAxis::Vertical,
                                  ImVec2(geo.leftActivityW, geo.curY + geo.mainH - 4.0F), ImVec2(geo.bottomDockW, 8.0F));
            }

            const Input::RawInputSnapshot &inputSnapshot = inputRouter.Snapshot();
            const bool inputBlocked = ImGui::GetDragDropPayload() != nullptr || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
            const WorkspaceSplitterInteractionResult splitter =
                splitterInteraction.Update(std::span<const WorkspaceSplitterRegion>(splitterRegions.data(), splitterRegionCount),
                                           WorkspaceSplitterPointerInput{.x = inputSnapshot.pointer.x,
                                                                         .y = inputSnapshot.pointer.y,
                                                                         .deltaX = inputSnapshot.pointer.deltaX,
                                                                         .deltaY = inputSnapshot.pointer.deltaY,
                                                                         .primaryClicked =
                                                                             inputSnapshot.State(Input::PointerButton::Primary).pressed &&
                                                                             !inputBlocked,
                                                                         .primaryDown =
                                                                             inputSnapshot.State(Input::PointerButton::Primary).down &&
                                                                             !inputBlocked},
                                           inputRouter, workspaceInputContext);
            if (splitter.axis == WorkspaceSplitterAxis::Horizontal) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            } else if (splitter.axis == WorkspaceSplitterAxis::Vertical) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            return splitter;
        }

        void DrawPanelAllocationTargets(const EditorWorkspaceView::WorkspaceLayoutGeometry &geo, const EditorWorkspaceViewModel &viewModel,
                                        const WorkspacePanelRegistry &panelRegistry, EditorWorkspaceViewCommandData &outCommand,
                                        const bool panelDragEligible) {
            const ImGuiPayload *payload = ImGui::GetDragDropPayload();
            if (payload == nullptr || std::strcmp(payload->DataType, "HORO_ACTIVITY_BAR_PANEL") != 0 || payload->Data == nullptr ||
                payload->DataSize <= 1) {
                return;
            }

            const std::string_view draggedPanelId(static_cast<const char *>(payload->Data));
            if (const bool knownPanel = std::ranges::any_of(panelRegistry.GetAllPanels(),
                                                            [draggedPanelId](const auto &panel) {
                return panel->GetId() == draggedPanelId;
            });
                !knownPanel) {
                return;
            }

            const float retainedBottomH =
                (std::min)((std::max)(0.0F, viewModel.bottomPanelHeight), (std::max)(0.0F, geo.activityBarH - kMinimumMainH));
            const float retainedMainH = (std::max)(0.0F, geo.activityBarH - retainedBottomH);
            const float retainedLeftW =
                (std::min)((std::max)(0.0F, viewModel.leftPanelWidth), (std::max)(0.0F, geo.availableDockW - kMinimumDocumentW));
            const float retainedRightW = (std::min)((std::max)(0.0F, viewModel.rightPanelWidth),
                                                    (std::max)(0.0F, geo.availableDockW - retainedLeftW - kMinimumDocumentW));
            const float retainedDocumentW = (std::max)(0.0F, geo.availableDockW - retainedLeftW - retainedRightW);

            const auto appendIndex = [&viewModel](const ActivityBarRail rail, const std::size_t groupIndex) {
                return viewModel.activityBarLayout.Groups(rail)[groupIndex].items.size();
            };
            const float retainedBottomHalfW = geo.availableDockW * 0.5F;
            const float retainedMainHalfH = retainedMainH * 0.5F;
            const float retainedRightX = geo.display.x - geo.rightActivityW - retainedRightW;
            constexpr float mergeHalfSpan = 8.0F;
            const float actualRightX = geo.display.x - geo.rightActivityW - geo.inspectorW;
            const std::array<AllocationTarget, 10> targets =
                {AllocationTarget{"##ActivityPanelPreviewLeftTop", WorkspaceDockArea::Left,
                                  ActivityBarSlot{ActivityBarRail::Left, 0, appendIndex(ActivityBarRail::Left, 0)}, std::nullopt,
                                  SideDockSlot::Top, ImVec2(geo.leftActivityW, geo.curY), ImVec2(retainedLeftW, retainedMainHalfH),
                                  ImVec2(geo.leftActivityW, geo.curY), ImVec2(retainedLeftW, retainedMainHalfH)},
                 AllocationTarget{"##ActivityPanelPreviewLeftBottom", WorkspaceDockArea::Left,
                                  ActivityBarSlot{ActivityBarRail::Left, 1, appendIndex(ActivityBarRail::Left, 1)}, std::nullopt,
                                  SideDockSlot::Bottom, ImVec2(geo.leftActivityW, geo.curY + retainedMainHalfH),
                                  ImVec2(retainedLeftW, retainedMainH - retainedMainHalfH),
                                  ImVec2(geo.leftActivityW, geo.curY + retainedMainHalfH),
                                  ImVec2(retainedLeftW, retainedMainH - retainedMainHalfH)},
                 AllocationTarget{"##ActivityPanelPreviewDocument", WorkspaceDockArea::Document,
                                  ActivityBarSlot{ActivityBarRail::DocumentTop, 0, appendIndex(ActivityBarRail::DocumentTop, 0)},
                                  std::nullopt, std::nullopt, ImVec2(geo.leftActivityW + retainedLeftW, geo.curY),
                                  ImVec2(retainedDocumentW, retainedMainH), ImVec2(geo.leftActivityW + retainedLeftW, geo.curY),
                                  ImVec2(retainedDocumentW, retainedMainH)},
                 AllocationTarget{"##ActivityPanelPreviewRightTop", WorkspaceDockArea::Right,
                                  ActivityBarSlot{ActivityBarRail::Right, 0, appendIndex(ActivityBarRail::Right, 0)}, std::nullopt,
                                  SideDockSlot::Top, ImVec2(retainedRightX, geo.curY), ImVec2(retainedRightW, retainedMainHalfH),
                                  ImVec2(retainedRightX, geo.curY), ImVec2(retainedRightW, retainedMainHalfH)},
                 AllocationTarget{"##ActivityPanelPreviewRightBottom", WorkspaceDockArea::Right,
                                  ActivityBarSlot{ActivityBarRail::Right, 1, appendIndex(ActivityBarRail::Right, 1)}, std::nullopt,
                                  SideDockSlot::Bottom, ImVec2(retainedRightX, geo.curY + retainedMainHalfH),
                                  ImVec2(retainedRightW, retainedMainH - retainedMainHalfH),
                                  ImVec2(retainedRightX, geo.curY + retainedMainHalfH),
                                  ImVec2(retainedRightW, retainedMainH - retainedMainHalfH)},
                 AllocationTarget{"##ActivityPanelPreviewBottomLeft", WorkspaceDockArea::Bottom,
                                  ActivityBarSlot{ActivityBarRail::Left, 2, appendIndex(ActivityBarRail::Left, 2)}, BottomDockSlot::Left,
                                  std::nullopt, ImVec2(geo.leftActivityW, geo.curY + retainedMainH),
                                  ImVec2(retainedBottomHalfW, retainedBottomH), ImVec2(geo.leftActivityW, geo.curY + retainedMainH),
                                  ImVec2(retainedBottomHalfW, retainedBottomH)},
                 AllocationTarget{"##ActivityPanelPreviewBottomRight", WorkspaceDockArea::Bottom,
                                  ActivityBarSlot{ActivityBarRail::Right, 2, appendIndex(ActivityBarRail::Right, 2)}, BottomDockSlot::Right,
                                  std::nullopt, ImVec2(geo.leftActivityW + retainedBottomHalfW, geo.curY + retainedMainH),
                                  ImVec2(geo.availableDockW - retainedBottomHalfW, retainedBottomH),
                                  ImVec2(geo.leftActivityW + retainedBottomHalfW, geo.curY + retainedMainH),
                                  ImVec2(geo.availableDockW - retainedBottomHalfW, retainedBottomH)},
                 AllocationTarget{"##ActivityPanelMergeLeft", WorkspaceDockArea::Left,
                                  ActivityBarSlot{ActivityBarRail::Left, 0, appendIndex(ActivityBarRail::Left, 0)}, std::nullopt,
                                  std::nullopt, ImVec2(geo.leftActivityW, geo.curY + geo.mainH * 0.5F - mergeHalfSpan),
                                  ImVec2(geo.hierarchyW, mergeHalfSpan * 2.0F), ImVec2(geo.leftActivityW, geo.curY),
                                  ImVec2(geo.hierarchyW, geo.mainH), true},
                 AllocationTarget{"##ActivityPanelMergeRight", WorkspaceDockArea::Right,
                                  ActivityBarSlot{ActivityBarRail::Right, 0, appendIndex(ActivityBarRail::Right, 0)}, std::nullopt,
                                  std::nullopt, ImVec2(actualRightX, geo.curY + geo.mainH * 0.5F - mergeHalfSpan),
                                  ImVec2(geo.inspectorW, mergeHalfSpan * 2.0F), ImVec2(actualRightX, geo.curY),
                                  ImVec2(geo.inspectorW, geo.mainH), true},
                 AllocationTarget{"##ActivityPanelMergeBottom", WorkspaceDockArea::Bottom,
                                  ActivityBarSlot{ActivityBarRail::Left, 2, appendIndex(ActivityBarRail::Left, 2)}, std::nullopt,
                                  std::nullopt, ImVec2(geo.leftActivityW + geo.availableDockW * 0.5F - mergeHalfSpan, geo.curY + geo.mainH),
                                  ImVec2(mergeHalfSpan * 2.0F, geo.contentH), ImVec2(geo.leftActivityW, geo.curY + geo.mainH),
                                  ImVec2(geo.availableDockW, geo.contentH), true}};

            for (const AllocationTarget &target : targets) {
                DrawAllocationTarget(target, viewModel, outCommand, panelDragEligible);
            }
        }

        void HandleSplitterResize(const WorkspaceSplitterInteractionResult &splitter,
                                  const EditorWorkspaceView::WorkspaceLayoutGeometry &geo, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &outCommand) {
            if (splitter.delta == 0.0F) {
                return;
            }

            if (splitter.active == WorkspaceSplitterId::Left) {
                outCommand.command = EditorWorkspaceViewCommand::ResizePanel;
                outCommand.targetIndex = 0;
                outCommand.floatPayload =
                    std::max(100.0f, std::min(geo.display.x - geo.leftActivityW - geo.rightActivityW - geo.inspectorW - 100.0f,
                                              viewModel.leftPanelWidth + splitter.delta));
                outCommand.layoutPayload =
                    WorkspaceLayoutSize{.leftWidth = *outCommand.floatPayload,
                                        .leftHeight = geo.mainH,
                                        .rightWidth = geo.inspectorW,
                                        .rightHeight = geo.mainH,
                                        .bottomWidth = geo.bottomDockW,
                                        .bottomHeight = geo.contentH,
                                        .documentWidth = geo.display.x - geo.leftActivityW - *outCommand.floatPayload - geo.inspectorW -
                                                         geo.rightActivityW,
                                        .documentHeight = geo.mainH};
            } else if (splitter.active == WorkspaceSplitterId::Right) {
                outCommand.command = EditorWorkspaceViewCommand::ResizePanel;
                outCommand.targetIndex = 1;
                outCommand.floatPayload =
                    std::max(100.0f, std::min(geo.display.x - geo.leftActivityW - geo.rightActivityW - geo.hierarchyW - 100.0f,
                                              viewModel.rightPanelWidth - splitter.delta));
                outCommand.layoutPayload = WorkspaceLayoutSize{.leftWidth = geo.hierarchyW,
                                                               .leftHeight = geo.mainH,
                                                               .rightWidth = *outCommand.floatPayload,
                                                               .rightHeight = geo.mainH,
                                                               .bottomWidth = geo.bottomDockW,
                                                               .bottomHeight = geo.contentH,
                                                               .documentWidth = geo.display.x - geo.leftActivityW - geo.hierarchyW -
                                                                                *outCommand.floatPayload - geo.rightActivityW,
                                                               .documentHeight = geo.mainH};
            } else if (splitter.active == WorkspaceSplitterId::Bottom) {
                outCommand.command = EditorWorkspaceViewCommand::ResizePanel;
                outCommand.targetIndex = 2;
                outCommand.floatPayload =
                    std::max(100.0f, std::min(geo.activityBarH - 100.0f, viewModel.bottomPanelHeight - splitter.delta));
                const float newMainH = geo.activityBarH - *outCommand.floatPayload;
                outCommand.layoutPayload = WorkspaceLayoutSize{.leftWidth = geo.hierarchyW,
                                                               .leftHeight = newMainH,
                                                               .rightWidth = geo.inspectorW,
                                                               .rightHeight = newMainH,
                                                               .bottomWidth = geo.bottomDockW,
                                                               .bottomHeight = *outCommand.floatPayload,
                                                               .documentWidth = geo.centerW,
                                                               .documentHeight = newMainH};
            }
        }
    }  // namespace

    EditorWorkspaceView::EditorWorkspaceView(const EditorGuiContext &context, const WorkspacePanelRegistry &panelRegistry,
                                             const std::uintptr_t logoTexture, Input::InputRouter &inputRouter,
                                             Input::InputContextToken &workspaceInputContext)
        : m_context(context), m_panelRegistry(panelRegistry), m_logoTexture(logoTexture), m_inputRouter(inputRouter),
          m_workspaceInputContext(workspaceInputContext) {}

    bool EditorWorkspaceView::EnsurePanelDragCapture() {
        if (m_panelDragCapture.IsActive()) {
            return true;
        }
        if (!m_inputRouter.Snapshot().State(Input::PointerButton::Primary).down) {
            return false;
        }
        m_panelDragContext =
            m_inputRouter.PushContext(Input::InputContextId{"editor.workspace.panel_drag"}, Input::InputContextKind::EditorToolCapture);
        Result<Input::PointerCaptureToken> captured =
            m_inputRouter.CapturePointer(m_panelDragContext, Input::PointerButton::Primary, *this);
        if (captured.HasError()) {
            m_panelDragContext.Reset();
            return false;
        }
        m_panelDragCapture = std::move(captured).Value();
        return true;
    }

    bool EditorWorkspaceView::PanelDragEligible() const noexcept {
        return m_panelDragCapture.IsActive() && m_inputRouter.IsContextActive(m_panelDragContext);
    }

    /** @copydoc EditorWorkspaceView::DrawActivityPanelDragSource */
    void EditorWorkspaceView::DrawActivityPanelDragSource(const std::string &panelId, const std::shared_ptr<IWorkspacePanel> &panel) {
        if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            return;
        if (EnsurePanelDragCapture()) {
            ImGui::SetDragDropPayload("HORO_ACTIVITY_BAR_PANEL", panelId.c_str(), panelId.size() + 1);
            ImGui::TextUnformatted(panel->GetDisplayName().c_str());
        }
        ImGui::EndDragDropSource();
    }

    void EditorWorkspaceView::OnInputCaptureCancelled(Input::CaptureCancellationReason) noexcept {
        m_panelDragCapture.Release();
        m_panelDragContext.Reset();
        m_panelDragCandidateId.clear();
    }

    void EditorWorkspaceView::Draw(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                                   const GuiContentRegion &contentRegion) {
        if (!m_inputRouter.Snapshot().State(Input::PointerButton::Primary).down) {
            m_panelDragCapture.Release();
            m_panelDragContext.Reset();
            m_panelDragCandidateId.clear();
        }
        const ImVec2 display{contentRegion.width, contentRegion.height};

        const float menuH = UsesNativeEditorMenuBar() ? 0.0F : kMenuBarH;
        const float recoveryH = viewModel.recoveryAvailable ? kRecoveryBarH : 0.0F;
        const float externalConflictH = viewModel.sceneExternalConflict ? kRecoveryBarH : 0.0F;
        // The shell has already removed its persistent status-bar height.
        const float activityBarH = (std::max)(0.0F, display.y - menuH - recoveryH - externalConflictH);

        const bool bottomDockActive = viewModel.bottomDockMode == BottomDockMode::Full
                                          ? !viewModel.activeBottomPanelId.empty()
                                          : !viewModel.activeBottomLeftPanelId.empty() || !viewModel.activeBottomRightPanelId.empty();
        const float contentH =
            !bottomDockActive ? 0.0F
                              : (std::max)(0.0F, (std::min)(viewModel.bottomPanelHeight, (std::max)(0.0F, activityBarH - kMinimumMainH)));

        // Main row height (Hierarchy, Viewport, Inspector)
        const float mainH = (std::max)(0.0F, activityBarH - contentH);

        constexpr float leftActivityW = kActivityBarW;
        constexpr float rightActivityW = kActivityBarW;
        const float availableDockW = (std::max)(0.0F, display.x - leftActivityW - rightActivityW);
        const bool leftDockActive = viewModel.leftDockMode == SideDockMode::Full
                                        ? !viewModel.activeLeftPanelId.empty()
                                        : !viewModel.activeLeftTopPanelId.empty() || !viewModel.activeLeftBottomPanelId.empty();
        const bool rightDockActive = viewModel.rightDockMode == SideDockMode::Full
                                         ? !viewModel.activeRightPanelId.empty()
                                         : !viewModel.activeRightTopPanelId.empty() || !viewModel.activeRightBottomPanelId.empty();
        float hierarchyW = leftDockActive ? (std::max)(0.0F, viewModel.leftPanelWidth) : 0.0F;
        float inspectorW = rightDockActive ? (std::max)(0.0F, viewModel.rightPanelWidth) : 0.0F;

        hierarchyW = (std::min)(hierarchyW, (std::max)(0.0F, availableDockW - kMinimumDocumentW));
        inspectorW = (std::min)(inspectorW, (std::max)(0.0F, availableDockW - hierarchyW - kMinimumDocumentW));

        const float centerW = (std::max)(0.0F, availableDockW - hierarchyW - inspectorW);
        const float bottomDockW = availableDockW;

        float curY = 0.0F;

        // ── Menu bar ────────────────────────────────────────────────────
        if (menuH > 0.0F) {
            DrawMenuBar(display, viewModel, outCommand);
            if (!m_inputRouter.IsContextActive(m_workspaceInputContext)) {
                outCommand.menuInvocation.reset();
            }
        }
        curY += menuH;

        if (viewModel.recoveryAvailable) {
            DrawRecoveryBar(ImVec2(0.0F, curY), ImVec2(display.x, recoveryH), outCommand);
            curY += recoveryH;
        }
        if (viewModel.sceneExternalConflict) {
            DrawExternalConflictBar(ImVec2(0.0F, curY), ImVec2(display.x, externalConflictH), outCommand);
            curY += externalConflictH;
        }

        const WorkspaceLayoutGeometry geo{
            .display = display,
            .curY = curY,
            .leftActivityW = leftActivityW,
            .rightActivityW = rightActivityW,
            .hierarchyW = hierarchyW,
            .inspectorW = inspectorW,
            .centerW = centerW,
            .bottomDockW = bottomDockW,
            .availableDockW = availableDockW,
            .mainH = mainH,
            .contentH = contentH,
            .activityBarH = activityBarH,
        };

        const WorkspaceSplitterInteractionResult splitter =
            UpdateSplitters(geo, m_splitterInteraction, m_inputRouter, m_workspaceInputContext);

        // ── Left Activity Bar ───────────────────────────────────────────
        DrawActivityBar(ImVec2(0.0F, curY), ImVec2(leftActivityW, activityBarH), m_panelRegistry, viewModel, outCommand,
                        {WorkspaceDockArea::Left, false, !m_splitterInteraction.OwnsPrimaryPointer()});

        // ── Middle Row and Bottom Dock ──────────────────────────────────
        DrawMiddleAndBottomDocks(geo, viewModel, outCommand);

        // ── Right Activity Bar ──────────────────────────────────────────
        DrawActivityBar(ImVec2(display.x - rightActivityW, curY), ImVec2(rightActivityW, activityBarH), m_panelRegistry, viewModel,
                        outCommand, {WorkspaceDockArea::Right, true, !m_splitterInteraction.OwnsPrimaryPointer()});

        // ── Allocation & merge drop targets ─────────────────────────────
        DrawPanelAllocationTargets(geo, viewModel, m_panelRegistry, outCommand, PanelDragEligible());

        // ── Splitter Resizing ───────────────────────────────────────────
        HandleSplitterResize(splitter, geo, viewModel, outCommand);
    }

    namespace {
        [[nodiscard]] bool IsFallbackMenuItemEnabled(const EditorMenuItem &item, const EditorWorkspaceViewModel &viewModel) {
            using enum EditorMenuAction;
            if (!item.enabledByDefault) {
                return false;
            }
            if (item.action == SaveScene) {
                return viewModel.isDirty;
            }
            if (item.action == Undo) {
                return viewModel.canUndo;
            }
            if (item.action == Redo) {
                return viewModel.canRedo;
            }
            return true;
        }

        void DrawFallbackMenuChildren(const EditorMenuItem &parent, const EditorWorkspaceViewModel &viewModel,
                                      EditorWorkspaceViewCommandData &outCommand, const EditorGuiContext &context) {
            for (const EditorMenuItem &item : parent.children) {
                if (item.kind == EditorMenuItemKind::Separator) {
                    Ui::ContextMenuSeparator();
                    continue;
                }

                const std::string &label = context.localization.Get("editor", item.labelKey);
                if (item.kind == EditorMenuItemKind::Submenu) {
                    if (Ui::BeginContextSubmenu(label.c_str(), context.theme.fonts, item.iconToken)) {
                        DrawFallbackMenuChildren(item, viewModel, outCommand, context);
                        Ui::EndContextSubmenu();
                    }
                    continue;
                }

                const bool enabled = IsFallbackMenuItemEnabled(item, viewModel);
                if (const char *shortcut = item.shortcut.empty() ? nullptr : item.shortcut.data();
                    Ui::ContextMenuItem(label.c_str(), shortcut, context.theme.fonts, Ui::ContextMenuItemTone::Normal, item.iconToken,
                                        enabled)) {
                    outCommand.menuInvocation = EditorMenuInvocation{item.action, item.primitive};
                }
            }
        }
    }  // namespace

    void EditorWorkspaceView::DrawMenuBar(const ImVec2 &display, const EditorWorkspaceViewModel &viewModel,
                                          EditorWorkspaceViewCommandData &outCommand) const {
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(display.x, kMenuBarH));
        ImGui::SetNextWindowBgAlpha(1.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg0());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Bg2());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 7.0F));

        ImGui::Begin("##MenuBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            constexpr ImVec2 logoSize(26.0F, 26.0F);
            const ImVec2 logoMin = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##HoroAppLogo", logoSize)) {
                outCommand.menuInvocation = EditorMenuInvocation{EditorMenuAction::OpenProject, std::nullopt};
            }
            if (m_logoTexture != 0) {
                ImGui::GetWindowDrawList()->AddImage(m_logoTexture, logoMin, ImVec2(logoMin.x + logoSize.x, logoMin.y + logoSize.y));
            } else {
                ImGui::GetWindowDrawList()->AddText(logoMin, Theme::U32(Theme::Accent()), "HORO");
            }
            ImGui::SameLine(0.0F, 10.0F);

            for (const EditorMenuItem &menu : GetEditorMenuModel().menus) {
                const std::string &label = m_context.localization.Get("editor", menu.labelKey);
                if (Ui::BeginMenuDropdown(label.c_str(), m_context.theme.fonts)) {
                    DrawFallbackMenuChildren(menu, viewModel, outCommand, m_context);
                    Ui::EndMenuDropdown();
                }
            }

            const std::string version = std::format("Horo Engine {}", HORO_ENGINE_VERSION_STRING);
            const float versionWidth = ImGui::CalcTextSize(version.c_str()).x;
            if (const float versionX = display.x - versionWidth - 12.0F; ImGui::GetCursorPosX() + 12.0F < versionX) {
                ImGui::SetCursorPosX(versionX);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
                ImGui::TextUnformatted(version.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::EndMenuBar();
        }

        const ImVec2 windowPos = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(windowPos.x, windowPos.y + kMenuBarH - 1.0F),
                                            ImVec2(windowPos.x + display.x, windowPos.y + kMenuBarH - 1.0F), Theme::U32(Theme::Border()));
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(3);
    }

    void EditorWorkspaceView::DrawRecoveryBar(const ImVec2 &pos, const ImVec2 &size, EditorWorkspaceViewCommandData &outCommand) const {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::SetNextWindowBgAlpha(1.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Warn());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 0.0F));
        ImGui::Begin("##SceneRecoveryBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings);

        const std::string &message = m_context.localization.Get("editor", "workspace.recovery.available");
        const std::string &restore = m_context.localization.Get("editor", "workspace.recovery.restore");
        const std::string &discard = m_context.localization.Get("editor", "workspace.recovery.discard");
        constexpr float buttonWidth = 96.0F;
        constexpr float buttonHeight = 28.0F;
        constexpr float gap = 8.0F;
        const float buttonY = pos.y + (size.y - buttonHeight) * 0.5F;
        const float discardX = pos.x + size.x - 12.0F - buttonWidth;
        const float restoreX = discardX - gap - buttonWidth;
        const float messageMaxX = restoreX - 12.0F;

        ImGui::PushClipRect(ImVec2(pos.x + 12.0F, pos.y), ImVec2((std::max)(pos.x + 12.0F, messageMaxX), pos.y + size.y), true);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0F, pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5F));
        ImGui::TextUnformatted(message.c_str());
        ImGui::PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(restoreX, buttonY));
        if (Ui::Button(Ui::ButtonProps{
                .label = restore.c_str(),
                .size = ImVec2(buttonWidth, buttonHeight),
                .variant = Ui::ButtonVariant::Primary,
                .componentSize = Ui::ComponentSize::Small,
            })) {
            outCommand.command = EditorWorkspaceViewCommand::RestoreSceneRecovery;
        }
        ImGui::SetCursorScreenPos(ImVec2(discardX, buttonY));
        if (Ui::Button(Ui::ButtonProps{
                .label = discard.c_str(),
                .size = ImVec2(buttonWidth, buttonHeight),
                .variant = Ui::ButtonVariant::Secondary,
                .componentSize = Ui::ComponentSize::Small,
            })) {
            outCommand.command = EditorWorkspaceViewCommand::DiscardSceneRecovery;
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    void EditorWorkspaceView::DrawExternalConflictBar(const ImVec2 &pos, const ImVec2 &size,
                                                      EditorWorkspaceViewCommandData &outCommand) const {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::SetNextWindowBgAlpha(1.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Warn());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 0.0F));
        ImGui::Begin("##SceneExternalConflictBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings);

        const std::string &message = m_context.localization.Get("editor", "workspace.scene_conflict.available");
        const std::string &reload = m_context.localization.Get("editor", "workspace.scene_conflict.reload");
        const std::string &compare = m_context.localization.Get("editor", "workspace.scene_conflict.compare");
        const std::string &overwrite = m_context.localization.Get("editor", "workspace.scene_conflict.overwrite");
        constexpr float actionButtonWidth = 128.0F;
        constexpr float compareButtonWidth = 96.0F;
        constexpr float buttonHeight = 28.0F;
        constexpr float gap = 8.0F;
        const float buttonY = pos.y + (size.y - buttonHeight) * 0.5F;
        const float overwriteX = pos.x + size.x - 12.0F - actionButtonWidth;
        const float reloadX = overwriteX - gap - actionButtonWidth;
        const float compareX = reloadX - gap - compareButtonWidth;
        const float messageMaxX = compareX - 12.0F;

        ImGui::PushClipRect(ImVec2(pos.x + 12.0F, pos.y), ImVec2((std::max)(pos.x + 12.0F, messageMaxX), pos.y + size.y), true);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0F, pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5F));
        ImGui::TextUnformatted(message.c_str());
        ImGui::PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(compareX, buttonY));
        if (Ui::Button(Ui::ButtonProps{
                .label = compare.c_str(),
                .size = ImVec2(compareButtonWidth, buttonHeight),
                .variant = Ui::ButtonVariant::Secondary,
                .componentSize = Ui::ComponentSize::Small,
            })) {
            outCommand.command = EditorWorkspaceViewCommand::CompareExternalScene;
        }
        ImGui::SetCursorScreenPos(ImVec2(reloadX, buttonY));
        if (Ui::Button(Ui::ButtonProps{
                .label = reload.c_str(),
                .size = ImVec2(actionButtonWidth, buttonHeight),
                .variant = Ui::ButtonVariant::Secondary,
                .componentSize = Ui::ComponentSize::Small,
            })) {
            outCommand.command = EditorWorkspaceViewCommand::ReloadExternalScene;
        }
        ImGui::SetCursorScreenPos(ImVec2(overwriteX, buttonY));
        if (Ui::Button(Ui::ButtonProps{
                .label = overwrite.c_str(),
                .size = ImVec2(actionButtonWidth, buttonHeight),
                .variant = Ui::ButtonVariant::Primary,
                .componentSize = Ui::ComponentSize::Small,
            })) {
            outCommand.command = EditorWorkspaceViewCommand::OverwriteExternalScene;
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    void EditorWorkspaceView::DrawDockArea(const WorkspaceDockArea area, const char *windowId, const ImVec2 &pos, const ImVec2 &size,
                                           const std::string_view activePanelId, const EditorWorkspaceViewModel &viewModel,
                                           EditorWorkspaceViewCommandData &outCommand) {
        // A screen transition, minimize, or sufficiently narrow host window can temporarily leave a dock with no
        // drawable area. InvisibleButton requires both dimensions to be non-zero, so defer the dock until layout
        // produces a usable rectangle on a later frame.
        if (!(size.x > 0.0F) || !(size.y > 0.0F)) {
            return;
        }

        const auto &panels = m_panelRegistry.GetAllPanels();

        BeginWorkspaceSurface(windowId, pos, size, {0.0F, 0.0F},
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings);

        std::shared_ptr<IWorkspacePanel> activePanel = nullptr;
        for (const auto &p : panels) {
            if (p->GetId() == activePanelId) {
                activePanel = p;
                break;
            }
        }

        if (!activePanel && area != WorkspaceDockArea::Document) {
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            return;
        }

        const char *targetNodeId = "workspace.document";
        if (area == WorkspaceDockArea::Left) {
            targetNodeId = "workspace.left";
        } else if (area == WorkspaceDockArea::Right) {
            targetNodeId = "workspace.right";
        } else if (area == WorkspaceDockArea::Bottom) {
            targetNodeId = "workspace.bottom";
        }
        if (const ImGuiPayload *dragPayload = ImGui::GetDragDropPayload();
            dragPayload != nullptr && dragPayload->IsDataType("HORO_WORKSPACE_PANEL")) {
            constexpr float edgeFraction = 0.22F;
            const float edgeW = size.x * edgeFraction;
            const float edgeH = size.y * edgeFraction;
            using enum WorkspacePanelHost::DropKind;
            if (area != WorkspaceDockArea::Document) {
                DrawWorkspaceDropTarget(targetNodeId, "##DropLeft", pos, ImVec2(edgeW, size.y), SplitLeft, outCommand);
                DrawWorkspaceDropTarget(targetNodeId, "##DropRight", ImVec2(pos.x + size.x - edgeW, pos.y), ImVec2(edgeW, size.y),
                                        SplitRight, outCommand);
                DrawWorkspaceDropTarget(targetNodeId, "##DropTop", ImVec2(pos.x + edgeW, pos.y), ImVec2(size.x - edgeW * 2.0F, edgeH),
                                        SplitTop, outCommand);
                DrawWorkspaceDropTarget(targetNodeId, "##DropBottom", ImVec2(pos.x + edgeW, pos.y + size.y - edgeH),
                                        ImVec2(size.x - edgeW * 2.0F, edgeH), SplitBottom, outCommand);
            }
            DrawWorkspaceDropTarget(targetNodeId, "##DropCenter", ImVec2(pos.x + edgeW, pos.y + edgeH),
                                    ImVec2(size.x - edgeW * 2.0F, size.y - edgeH * 2.0F), TabCenter, outCommand);
        }

        const float tabHeight = area == WorkspaceDockArea::Document ? 28.0F * Theme::GetActiveTokens().sizes.uiScale : 0.0F;
        if (area == WorkspaceDockArea::Document)
            DrawDocumentTabs(viewModel, outCommand);

        ImGui::SetCursorPos(ImVec2(0.0F, 0.0F));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::SetCursorPosY(tabHeight);
        ImGui::BeginChild("##DockContent", ImVec2(0.0F, size.y - tabHeight), false, ImGuiWindowFlags_NoSavedSettings);
        if (activePanel)
            activePanel->DrawPanel(ImGui::GetWindowPos(), ImGui::GetWindowSize(), viewModel, outCommand, m_context);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Preserve panel rearrangement without adding visible host chrome. The
        // panel-owned top tab/title region doubles as the drag initiation area.
        const float dragRegionHeight = tabHeight > 0.0F ? tabHeight : 28.0F * Theme::GetActiveTokens().sizes.uiScale;
        if (const bool pointerInDragRegion = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + dragRegionHeight), false);
            activePanel && area != WorkspaceDockArea::Document && !m_splitterInteraction.OwnsPrimaryPointer() && pointerInDragRegion &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_panelDragCandidateId.assign(activePanelId);
        const ImGuiPayload *currentPayload = ImGui::GetDragDropPayload();
        if (const bool payloadAllowsPanelDrag = currentPayload == nullptr || currentPayload->IsDataType("HORO_WORKSPACE_PANEL");
            !m_splitterInteraction.OwnsPrimaryPointer() && m_panelDragCandidateId == activePanelId && payloadAllowsPanelDrag &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            if (EnsurePanelDragCapture()) {
                ImGui::SetDragDropPayload("HORO_WORKSPACE_PANEL", activePanelId.data(), activePanelId.size());
                ImGui::TextUnformatted(activePanelId.data(), activePanelId.data() + activePanelId.size());
            }
            ImGui::EndDragDropSource();
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    void EditorWorkspaceView::DrawDocumentTabs(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) {
        const TabStackNode *stack = viewModel.workspacePanelHost.Layout().FindTabStack("workspace.document");
        if (stack == nullptr)
            return;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0F, 5.0F));
        for (const std::string &panelId : stack->tabs) {
            const auto &panels = m_panelRegistry.GetAllPanels();
            const auto panel = std::ranges::find_if(panels, [&panelId](const auto &candidate) {
                return candidate->GetId() == panelId;
            });
            ImGui::PushID(panelId.c_str());
            const char *title =
                panel == panels.end() ? panelId.c_str() : m_context.localization.Get("editor", (*panel)->GetDisplayName()).c_str();
            if (stack->activeTab == panelId)
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::Bg2());
            if (ImGui::Button(title)) {
                outCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
                outCommand.targetIndex = 3;
                outCommand.stringPayload = panelId;
            }
            if (panel != panels.end())
                DrawActivityPanelDragSource(panelId, *panel);
            if (stack->activeTab == panelId)
                ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(26.0F, 0.0F))) {
                outCommand.command = EditorWorkspaceViewCommand::CloseWorkspacePanel;
                outCommand.stringPayload = panelId;
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        if (ImGui::Button("+##OpenDocumentTab"))
            ImGui::OpenPopup("##OpenDocumentTabMenu");
        if (ImGui::BeginPopup("##OpenDocumentTabMenu")) {
            for (const auto &panel : m_panelRegistry.GetAllPanels()) {
                if (std::ranges::find(stack->tabs, panel->GetId()) != stack->tabs.end())
                    continue;
                const std::string &title = m_context.localization.Get("editor", panel->GetDisplayName());
                if (Ui::ContextMenuItem(title.c_str(), nullptr, m_context.theme.fonts)) {
                    outCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
                    outCommand.targetIndex = 3;
                    outCommand.stringPayload = panel->GetId();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }

    void EditorWorkspaceView::DrawMiddleAndBottomDocks(const WorkspaceLayoutGeometry &geo, const EditorWorkspaceViewModel &viewModel,
                                                       EditorWorkspaceViewCommandData &outCommand) {
        using enum Horo::Editor::WorkspaceDockArea;
        float curX = geo.leftActivityW;

        // Left Dock
        if (geo.hierarchyW > 0.0F) {
            if (viewModel.leftDockMode == SideDockMode::Full) {
                DrawDockArea(WorkspaceDockArea::Left, "##DockLeft", ImVec2(curX, geo.curY), ImVec2(geo.hierarchyW, geo.mainH),
                             viewModel.activeLeftPanelId, viewModel, outCommand);
            } else {
                const float halfHeight = geo.mainH * 0.5F;
                DrawDockArea(WorkspaceDockArea::Left, "##DockLeftTop", ImVec2(curX, geo.curY), ImVec2(geo.hierarchyW, halfHeight),
                             viewModel.activeLeftTopPanelId, viewModel, outCommand);
                DrawDockArea(WorkspaceDockArea::Left, "##DockLeftBottom", ImVec2(curX, geo.curY + halfHeight),
                             ImVec2(geo.hierarchyW, geo.mainH - halfHeight), viewModel.activeLeftBottomPanelId, viewModel, outCommand);
            }
            curX += geo.hierarchyW;
        }

        // Document Dock
        DrawDockArea(WorkspaceDockArea::Document, "##DockDocument", ImVec2(curX, geo.curY), ImVec2(geo.centerW, geo.mainH),
                     viewModel.activeDocumentPanelId, viewModel, outCommand);
        curX += geo.centerW;

        // Right Dock
        if (geo.inspectorW > 0.0F) {
            if (viewModel.rightDockMode == SideDockMode::Full) {
                DrawDockArea(WorkspaceDockArea::Right, "##DockRight", ImVec2(curX, geo.curY), ImVec2(geo.inspectorW, geo.mainH),
                             viewModel.activeRightPanelId, viewModel, outCommand);
            } else {
                const float halfHeight = geo.mainH * 0.5F;
                DrawDockArea(WorkspaceDockArea::Right, "##DockRightTop", ImVec2(curX, geo.curY), ImVec2(geo.inspectorW, halfHeight),
                             viewModel.activeRightTopPanelId, viewModel, outCommand);
                DrawDockArea(WorkspaceDockArea::Right, "##DockRightBottom", ImVec2(curX, geo.curY + halfHeight),
                             ImVec2(geo.inspectorW, geo.mainH - halfHeight), viewModel.activeRightBottomPanelId, viewModel, outCommand);
            }
        }

        // Bottom Dock
        if (geo.contentH > 0.0F) {
            const ImVec2 bottomPos(geo.leftActivityW, geo.curY + geo.mainH);
            if (viewModel.bottomDockMode == BottomDockMode::Full) {
                DrawDockArea(WorkspaceDockArea::Bottom, "##DockBottom", bottomPos, ImVec2(geo.bottomDockW, geo.contentH),
                             viewModel.activeBottomPanelId, viewModel, outCommand);
            } else {
                const float halfWidth = geo.bottomDockW * 0.5F;
                if (!viewModel.activeBottomLeftPanelId.empty()) {
                    DrawDockArea(WorkspaceDockArea::Bottom, "##DockBottomLeft", bottomPos, ImVec2(halfWidth, geo.contentH),
                                 viewModel.activeBottomLeftPanelId, viewModel, outCommand);
                }
                if (!viewModel.activeBottomRightPanelId.empty()) {
                    DrawDockArea(WorkspaceDockArea::Bottom, "##DockBottomRight", ImVec2(bottomPos.x + halfWidth, bottomPos.y),
                                 ImVec2(geo.bottomDockW - halfWidth, geo.contentH), viewModel.activeBottomRightPanelId, viewModel,
                                 outCommand);
                }
            }
        }
    }

    void EditorWorkspaceView::DrawWorkspaceDropTarget(const char *targetNodeId, const char *id, const ImVec2 &position, const ImVec2 &size,
                                                      const WorkspacePanelHost::DropKind kind,
                                                      EditorWorkspaceViewCommandData &outCommand) const {
        ImGui::SetCursorScreenPos(position);
        ImGui::PushID(id);
        ImGui::InvisibleButton("##WorkspaceDropTarget", size);
        if (ImGui::IsItemHovered()) {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), Theme::U32(Theme::AccentSoft()), 4.0F);
            drawList->AddRect(position, ImVec2(position.x + size.x, position.y + size.y), Theme::U32(Theme::Accent()), 1.0F, 0, 2.0F);
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("HORO_WORKSPACE_PANEL");
                payload != nullptr && PanelDragEligible()) {
                outCommand.command = EditorWorkspaceViewCommand::DockWorkspacePanel;
                outCommand.stringPayload =
                    std::string(static_cast<const char *>(payload->Data), static_cast<std::size_t>(payload->DataSize));
                outCommand.workspaceDropTarget = WorkspacePanelDropTarget{targetNodeId, kind};
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    void EditorWorkspaceView::DrawActivityBarGroup(const ActivityBarGroupParams &params, const EditorWorkspaceViewModel &viewModel,
                                                   EditorWorkspaceViewCommandData &outCommand) {
        ImGui::PushClipRect(ImVec2(params.pos.x, params.geometry.contentY + params.groupTop),
                            ImVec2(params.pos.x + params.size.x, params.geometry.contentY + params.groupBottom), true);

        const ActivityBarRail rail = params.options.area == WorkspaceDockArea::Right ? ActivityBarRail::Right : ActivityBarRail::Left;
        float currentY = params.groupTop;

        if (params.group.items.empty()) {
            if (params.draggingActivityItem) {
                DrawActivityDropSlot(ActivityBarSlot{rail, params.groupIndex, 0}, currentY, params.draggingActivityItem, params.geometry,
                                     outCommand);
            }
            ImGui::PopClipRect();
            return;
        }

        auto findPanel = [this](const std::string_view panelId) -> std::shared_ptr<IWorkspacePanel> {
            for (const auto &panel : m_panelRegistry.GetAllPanels()) {
                if (panel->GetId() == panelId) {
                    return panel;
                }
            }
            return {};
        };

        for (std::size_t itemIndex = 0; itemIndex < params.group.items.size(); ++itemIndex) {
            if (DrawActivityDropSlot(ActivityBarSlot{rail, params.groupIndex, itemIndex}, currentY, params.draggingActivityItem,
                                     params.geometry, outCommand)) {
                currentY += params.geometry.cellHeight + params.geometry.cellGap;
            }

            const std::string &panelId = params.group.items[itemIndex];
            const auto panel = findPanel(panelId);
            if (!panel) {
                continue;
            }

            currentY = DrawActivityItem(panelId, currentY, params.geometry, viewModel, outCommand, params.options, panel);
        }

        DrawActivityDropSlot(ActivityBarSlot{rail, params.groupIndex, params.group.items.size()}, currentY, params.draggingActivityItem,
                             params.geometry, outCommand);

        ImGui::PopClipRect();
    }

    void EditorWorkspaceView::DrawActivityBar(const ImVec2 &pos, const ImVec2 &size, const WorkspacePanelRegistry &,
                                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                                              const ActivityBarOptions options) {
        const char *windowId = options.indicatorOnRight ? "##ActivityRight" : "##ActivityLeft";
        BeginWorkspaceSurface(windowId, pos, size, {0.0F, 6.0F},
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs |
                                  ImGuiWindowFlags_NoNavFocus);

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
        constexpr float activityBarBorder = 1.0F;
        constexpr float cellHeight = 30.0F;
        constexpr float cellGap = 2.0F;
        const float outerWidth = (std::max)(0.0F, size.x);
        const float cellX = pos.x + activityBarBorder;
        const float cellWidth = (std::max)(0.0F, outerWidth - 2.0F * activityBarBorder);
        const float contentY = windowPos.y + contentMin.y;
        const auto &groups =
            viewModel.activityBarLayout.Groups(options.area == WorkspaceDockArea::Right ? ActivityBarRail::Right : ActivityBarRail::Left);
        const bool draggingActivityItem = ImGui::GetDragDropPayload() != nullptr;

        constexpr float activityBarBottomPadding = 6.0F;
        const float usableHeight = (std::max)(0.0F, size.y - contentMin.y - activityBarBottomPadding);
        const ActivityBarGeometry geometry{cellX, contentY, cellWidth, cellHeight, cellGap, drawList};
        const float cellStride = cellHeight + cellGap;
        const auto groupExtent = [cellStride, draggingActivityItem](const ActivityBarGroup &group) {
            const std::size_t slotCount = group.items.size() + (draggingActivityItem ? 1U : 0U);
            return slotCount == 0U ? 0.0F : static_cast<float>(slotCount) * cellStride - cellGap;
        };

        float topGroupY = 0.0F;
        for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            const float extent = groupExtent(groups[groupIndex]);
            const bool bottomAnchored = groupIndex + 1U == groups.size();
            const float groupTop = bottomAnchored ? (std::max)(topGroupY, usableHeight - extent) : topGroupY;
            const float groupBottom = (std::min)(usableHeight, groupTop + extent);
            DrawActivityBarGroup(ActivityBarGroupParams{.groupIndex = groupIndex,
                                                        .group = groups[groupIndex],
                                                        .groupTop = groupTop,
                                                        .groupBottom = groupBottom,
                                                        .pos = pos,
                                                        .size = size,
                                                        .geometry = geometry,
                                                        .options = options,
                                                        .draggingActivityItem = draggingActivityItem},
                                 viewModel, outCommand);
            if (!bottomAnchored) {
                topGroupY = groupBottom + (extent > 0.0F ? cellGap : 0.0F);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    bool EditorWorkspaceView::DrawActivityDropSlot(const ActivityBarSlot slot, const float y, const bool draggingActivityItem,
                                                   const ActivityBarGeometry &geometry, EditorWorkspaceViewCommandData &outCommand) const {
        if (!draggingActivityItem) {
            return false;
        }

        ImGui::SetCursorScreenPos(ImVec2(geometry.cellX, geometry.contentY + y));
        ImGui::PushID(static_cast<int>(slot.groupIndex * 1000 + slot.itemIndex));
        ImGui::InvisibleButton("##ActivityInsertSlot", ImVec2(geometry.cellWidth, geometry.cellHeight));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const ImVec2 targetMin = ImGui::GetItemRectMin();
        const ImVec2 targetMax = ImGui::GetItemRectMax();
        if (hovered) {
            const ImVec2 placeholderMin(targetMin.x + 0.5F, targetMin.y + 0.5F);
            const ImVec2 placeholderMax(targetMax.x - 0.5F, targetMax.y - 0.5F);
            geometry.drawList->AddRectFilled(placeholderMin, placeholderMax, Theme::U32(Theme::AccentSoft()), 2.0F);
            geometry.drawList->AddRect(placeholderMin, placeholderMax, Theme::U32(Theme::Accent()), 1.0F, 0, 2.0F);
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload *payload =
                    ImGui::AcceptDragDropPayload("HORO_ACTIVITY_BAR_PANEL", ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                payload != nullptr && PanelDragEligible()) {
                outCommand.command = EditorWorkspaceViewCommand::ReorderActivityBarItem;
                outCommand.stringPayload = static_cast<const char *>(payload->Data);
                outCommand.activityBarSlot = slot;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return hovered;
    }

    float EditorWorkspaceView::DrawActivityItem(const std::string &panelId, const float y, const ActivityBarGeometry &geometry,
                                                const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand,
                                                const ActivityBarOptions options, const std::shared_ptr<IWorkspacePanel> &panel) {
        WorkspaceDockArea panelArea = panel->GetDefaultDockArea();
        if (const auto placement = viewModel.panelDockAreas.find(panelId); placement != viewModel.panelDockAreas.end()) {
            panelArea = placement->second;
        }

        const bool isActive = IsActivityItemActive(panelId, viewModel);
        const bool activeInBottomSplit = IsActiveInBottomSplit(panelId, viewModel);
        const bool activeInSideSplit = IsActiveInSideSplit(panelId, viewModel);
        const ImVec2 itemMin(geometry.cellX, geometry.contentY + y);
        const ImVec2 itemMax(geometry.cellX + geometry.cellWidth, geometry.contentY + y + geometry.cellHeight);
        ImGui::SetCursorScreenPos(itemMin);
        ImGui::PushID(panelId.c_str());
        if (ImGui::InvisibleButton("##ActivityItem", ImVec2(geometry.cellWidth, geometry.cellHeight))) {
            outCommand.command = EditorWorkspaceViewCommand::ChangeActivePanel;
            outCommand.targetIndex = ActivityBarAreaIndex(panelArea);
            outCommand.stringPayload = isActive && !activeInBottomSplit && !activeInSideSplit ? std::string{} : panelId;
        }
        if (options.allowDragSources)
            DrawActivityPanelDragSource(panelId, panel);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        if (isActive || hovered) {
            geometry.drawList->AddRectFilled(itemMin, itemMax, Theme::U32(isActive ? Theme::AccentSoft() : Theme::Hover()));
        }
        if (isActive) {
            constexpr float indicatorWidth = 2.0F;
            constexpr float indicatorInset = 3.0F;
            const float indicatorX = options.indicatorOnRight ? itemMax.x - indicatorWidth : itemMin.x;
            geometry.drawList->AddRectFilled(ImVec2(indicatorX, itemMin.y + indicatorInset),
                                             ImVec2(indicatorX + indicatorWidth, itemMax.y - indicatorInset), Theme::U32(Theme::Accent()));
        }
        const ImU32 iconColor = isActive || hovered ? Theme::U32(Theme::Text()) : Theme::U32(Theme::Muted());
        panel->DrawIcon(geometry.drawList, itemMin, ImVec2(geometry.cellWidth, geometry.cellHeight), iconColor);
        return y + geometry.cellHeight + geometry.cellGap;
    }
}  // namespace Horo::Editor
