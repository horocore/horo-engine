#include "editor/screens/workspace/panels/hierarchy/HierarchyPanel.h"

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/AssetSceneDrop.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyRowLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

namespace Horo::Editor {
    namespace {
        constexpr float kTabHeight = 36.0F;
        constexpr float kToolbarHeight = 42.0F;
        constexpr float kSearchRegionHeight = 30.0F;
        constexpr float kFooterHeight = 28.0F;
        constexpr float kOuterPadding = 6.0F;
        constexpr float kRowActionsWidth = 48.0F;

        /** @brief Glyph identities used by the hierarchy-local action strip. */
        enum class ToolbarGlyph : std::uint8_t {
            Create,
            Filter,
            Sort,
            More,
        };

        /** @brief Action requests emitted by the hierarchy toolbar for the current frame. */
        struct HierarchyToolbarResult {
            bool createPressed{false};
            bool focusSearchPressed{false};
        };

        /** @brief Draws a one-pixel dashed rectangle matching the hierarchy root drop target. */
        void DrawDashedRect(ImDrawList &drawList, const ImVec2 minimum, const ImVec2 maximum, const ImU32 color, const float scale) {
            const float dash = 4.0F * scale;
            const float step = 7.0F * scale;
            const auto horizontalDashCount = static_cast<int>(std::ceil((maximum.x - minimum.x) / step));
            for (int index = 0; index < horizontalDashCount; ++index) {
                const float x = minimum.x + static_cast<float>(index) * step;
                const float xEnd = std::min(maximum.x, x + dash);
                drawList.AddLine({x, minimum.y}, {xEnd, minimum.y}, color, scale);
                drawList.AddLine({x, maximum.y}, {xEnd, maximum.y}, color, scale);
            }
            const auto verticalDashCount = static_cast<int>(std::ceil((maximum.y - minimum.y) / step));
            for (int index = 0; index < verticalDashCount; ++index) {
                const float y = minimum.y + static_cast<float>(index) * step;
                const float yEnd = std::min(maximum.y, y + dash);
                drawList.AddLine({minimum.x, y}, {minimum.x, yEnd}, color, scale);
                drawList.AddLine({maximum.x, y}, {maximum.x, yEnd}, color, scale);
            }
        }

        /** @brief Substitutes the hierarchy object count into one complete localized label. */
        [[nodiscard]] std::string FormatObjectCount(std::string pattern, const std::size_t count) {
            constexpr std::string_view token{"{count}"};
            if (const std::size_t position = pattern.find(token); position != std::string::npos)
                pattern.replace(position, token.size(), std::to_string(count));
            return pattern;
        }

        /** @brief Draws the reference-height hierarchy tab without changing shared bottom-dock tabs. */
        void DrawHierarchyTab(const char *label, const Theme::Fonts &fonts, const float uiScale) {
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float tabHeight = kTabHeight * uiScale;
            ImDrawList &drawList = *ImGui::GetWindowDrawList();
            drawList.AddRectFilled(minimum, {minimum.x + width, minimum.y + tabHeight}, Theme::U32(Theme::Bg0()));
            drawList.AddLine({minimum.x, minimum.y + tabHeight - uiScale}, {minimum.x + width, minimum.y + tabHeight - uiScale},
                             Theme::U32(Theme::Border()));

            ImFont *font = fonts.sans != nullptr ? fonts.sans : ImGui::GetFont();
            const float fontSize = Theme::TextPx::Label();
            const ImVec2 textSize = font->CalcTextSizeA(fontSize, 100000.0F, 0.0F, label);
            const float dockPadding = 10.0F * uiScale;
            const float horizontalPadding = 10.0F * uiScale;
            const float tabWidth = textSize.x + horizontalPadding * 2.0F;
            drawList.AddText(font, fontSize, {minimum.x + dockPadding + horizontalPadding, minimum.y + (tabHeight - textSize.y) * 0.5F},
                             Theme::U32(Theme::Text()), label);
            drawList.AddRectFilled({minimum.x + dockPadding, minimum.y + tabHeight - 2.0F * uiScale},
                                   {minimum.x + dockPadding + tabWidth, minimum.y + tabHeight}, Theme::U32(Theme::Accent()));
            ImGui::Dummy({width, tabHeight});
            // A custom-drawn tab is immediately followed by a child region. Remove
            // ImGui's implicit inter-item gap so the toolbar starts at the CSS tab edge.
            ImGui::SetCursorScreenPos({minimum.x, minimum.y + tabHeight});
        }

        /** @brief Draws one hierarchy toolbar glyph inside a fixed 30-pixel button. */
        void DrawToolbarGlyph(ImDrawList &drawList, const ToolbarGlyph glyph, const ImVec2 minimum, const ImU32 color,
                              const float uiScale) {
            const ImVec2 origin{minimum.x + 7.0F * uiScale, minimum.y + 7.0F * uiScale};
            if (glyph == ToolbarGlyph::Create) {
                drawList.AddLine({origin.x + 8.0F * uiScale, origin.y + 3.0F * uiScale},
                                 {origin.x + 8.0F * uiScale, origin.y + 13.0F * uiScale}, color, 1.6F * uiScale);
                drawList.AddLine({origin.x + 3.0F * uiScale, origin.y + 8.0F * uiScale},
                                 {origin.x + 13.0F * uiScale, origin.y + 8.0F * uiScale}, color, 1.6F * uiScale);
            } else if (glyph == ToolbarGlyph::Filter) {
                const std::array points{ImVec2{origin.x + 2.5F * uiScale, origin.y + 3.0F * uiScale},
                                        ImVec2{origin.x + 13.5F * uiScale, origin.y + 3.0F * uiScale},
                                        ImVec2{origin.x + 9.3F * uiScale, origin.y + 8.0F * uiScale},
                                        ImVec2{origin.x + 9.3F * uiScale, origin.y + 12.2F * uiScale},
                                        ImVec2{origin.x + 6.7F * uiScale, origin.y + 13.2F * uiScale},
                                        ImVec2{origin.x + 6.7F * uiScale, origin.y + 8.0F * uiScale}};
                drawList.AddPolyline(points.data(), points.size(), color, ImDrawFlags_Closed, 1.4F * uiScale);
            } else if (glyph == ToolbarGlyph::Sort) {
                drawList.AddLine({origin.x + 2.0F * uiScale, origin.y + 4.0F * uiScale},
                                 {origin.x + 8.0F * uiScale, origin.y + 4.0F * uiScale}, color, 1.4F * uiScale);
                drawList.AddLine({origin.x + 2.0F * uiScale, origin.y + 8.0F * uiScale},
                                 {origin.x + 6.0F * uiScale, origin.y + 8.0F * uiScale}, color, 1.4F * uiScale);
                drawList.AddLine({origin.x + 2.0F * uiScale, origin.y + 12.0F * uiScale},
                                 {origin.x + 4.0F * uiScale, origin.y + 12.0F * uiScale}, color, 1.4F * uiScale);
                drawList.AddLine({origin.x + 11.0F * uiScale, origin.y + 3.0F * uiScale},
                                 {origin.x + 11.0F * uiScale, origin.y + 13.0F * uiScale}, color, 1.4F * uiScale);
                drawList.AddLine({origin.x + 9.0F * uiScale, origin.y + 11.0F * uiScale},
                                 {origin.x + 11.0F * uiScale, origin.y + 13.0F * uiScale}, color, 1.4F * uiScale);
                drawList.AddLine({origin.x + 11.0F * uiScale, origin.y + 13.0F * uiScale},
                                 {origin.x + 13.0F * uiScale, origin.y + 11.0F * uiScale}, color, 1.4F * uiScale);
            } else {
                drawList.AddCircleFilled({origin.x + 8.0F * uiScale, origin.y + 3.5F * uiScale}, uiScale, color);
                drawList.AddCircleFilled({origin.x + 8.0F * uiScale, origin.y + 8.0F * uiScale}, uiScale, color);
                drawList.AddCircleFilled({origin.x + 8.0F * uiScale, origin.y + 12.5F * uiScale}, uiScale, color);
            }
        }

        /** @brief Draws one hierarchy toolbar button and reports a primary-button activation. */
        bool DrawToolbarButton(const char *id, const ToolbarGlyph glyph, const ImVec2 minimum, const bool primary, const char *tooltip,
                               const float uiScale) {
            const ImVec2 size{30.0F * uiScale, 30.0F * uiScale};
            ImGui::SetCursorScreenPos(minimum);
            ImGui::InvisibleButton(id, size);
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList &drawList = *ImGui::GetWindowDrawList();
            if (primary || hovered) {
                drawList.AddRectFilled(minimum, {minimum.x + size.x, minimum.y + size.y}, Theme::U32(Theme::Bg2()), 4.0F * uiScale);
                drawList.AddRect(minimum, {minimum.x + size.x, minimum.y + size.y}, Theme::U32(Theme::Border()), 4.0F * uiScale);
            }
            DrawToolbarGlyph(drawList, glyph, minimum, Theme::U32(hovered ? Theme::Text() : Theme::Muted()), uiScale);
            if (hovered)
                Ui::ShowTooltip(tooltip);
            return ImGui::IsItemClicked(ImGuiMouseButton_Left);
        }

        /** @brief Draws the hierarchy action strip and returns its supported action requests. */
        HierarchyToolbarResult DrawHierarchyToolbar(const float panelWidth, const EditorGuiContext &context, const float uiScale) {
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const float toolbarHeight = kToolbarHeight * uiScale;
            const float buttonSize = 30.0F * uiScale;
            const float gap = 6.0F * uiScale;
            ImDrawList &drawList = *ImGui::GetWindowDrawList();
            drawList.AddRectFilled(minimum, {minimum.x + panelWidth, minimum.y + toolbarHeight}, Theme::U32(Theme::Bg1()));
            drawList.AddLine({minimum.x, minimum.y + toolbarHeight - uiScale},
                             {minimum.x + panelWidth, minimum.y + toolbarHeight - uiScale}, Theme::U32(Theme::Border()));

            HierarchyToolbarResult result;
            const float y = minimum.y + 6.0F * uiScale;
            result.createPressed =
                DrawToolbarButton("##HierarchyCreateButton", ToolbarGlyph::Create, {minimum.x + 8.0F * uiScale, y}, true,
                                  context.localization.Get("editor", "workspace.hierarchy.toolbar.create").c_str(), uiScale);
            const float right = minimum.x + panelWidth - 8.0F * uiScale;
            static_cast<void>(DrawToolbarButton("##HierarchyOptionsButton", ToolbarGlyph::More, {right - buttonSize, y}, false,
                                                context.localization.Get("editor", "workspace.hierarchy.toolbar.options").c_str(),
                                                uiScale));
            static_cast<void>(DrawToolbarButton("##HierarchySortButton", ToolbarGlyph::Sort, {right - buttonSize * 2.0F - gap, y}, false,
                                                context.localization.Get("editor", "workspace.hierarchy.toolbar.sort").c_str(), uiScale));
            result.focusSearchPressed =
                DrawToolbarButton("##HierarchyFilterButton", ToolbarGlyph::Filter, {right - buttonSize * 3.0F - gap * 2.0F, y}, false,
                                  context.localization.Get("editor", "workspace.hierarchy.toolbar.filter").c_str(), uiScale);
            ImGui::SetCursorScreenPos({minimum.x, minimum.y + toolbarHeight});
            ImGui::Dummy({panelWidth, 0.0F});
            return result;
        }

        [[nodiscard]] ImVec4 BlendColor(const ImVec4 &first, const ImVec4 &second, const float amount) noexcept {
            const float clamped = std::clamp(amount, 0.0F, 1.0F);
            return {
                first.x + (second.x - first.x) * clamped,
                first.y + (second.y - first.y) * clamped,
                first.z + (second.z - first.z) * clamped,
                first.w + (second.w - first.w) * clamped,
            };
        }

        struct HierarchyIconPresentation {
            Ui::UiIcon icon{Ui::UiIcon::SceneObject};
            const char *tooltipKey{nullptr};
            ImVec4 color{};
        };

        /** @brief Builds the shared icon presentation for scene objects. */
        [[nodiscard]] HierarchyIconPresentation SceneObjectIconPresentation(const char *tooltipKey) {
            return {.icon = Ui::UiIcon::SceneObject, .tooltipKey = tooltipKey, .color = Theme::Muted()};
        }

        [[nodiscard]] HierarchyIconPresentation GetIconPresentation(const HierarchyNodeType type) {
            using enum HierarchyNodeType;
            switch (type) {
                case Mesh:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.mesh");
                case Empty:
                case Collection:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.empty");
                case Light:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.light");
                case PointLight:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.light_point");
                case DirectionalLight:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.light_directional");
                case SpotLight:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.light_spot");
                case Camera:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.camera");
                case TriggerVolume:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.volume");
                case AudioSource:
                    return SceneObjectIconPresentation("workspace.hierarchy.type.audio");
            }
            return SceneObjectIconPresentation("workspace.hierarchy.type.empty");
        }

        [[nodiscard]] ImFont *ResolveFont(ImFont *preferred) {
            return preferred != nullptr ? preferred : ImGui::GetFont();
        }

        struct HierarchyLabelDrawRequest {
            ImDrawList &drawList;
            ImFont &font;
            float fontSize{0.0F};
            ImVec2 minimum{};
            ImVec2 maximum{};
            float centerY{0.0F};
            ImU32 color{};
            std::string_view text;
        };

        [[nodiscard]] bool DrawHierarchyLabel(const HierarchyLabelDrawRequest &request) {
            const auto &[drawList, font, fontSize, minimum, maximum, centerY, color, text] = request;
            if (text.empty() || maximum.x <= minimum.x)
                return !text.empty();
            const ImVec2 fullSize = font.CalcTextSizeA(fontSize, 100000.0F, 0.0F, text.data(), text.data() + text.size());
            const float availableWidth = maximum.x - minimum.x;
            const ImVec2 textPosition{minimum.x, centerY - fullSize.y * 0.5F};
            drawList.PushClipRect(minimum, maximum, true);
            if (fullSize.x <= availableWidth) {
                drawList.AddText(&font, fontSize, textPosition, color, text.data(), text.data() + text.size());
                drawList.PopClipRect();
                return false;
            }

            constexpr std::string_view ellipsis{"\xE2\x80\xA6"};
            const float ellipsisWidth = font.CalcTextSizeA(fontSize, 100.0F, 0.0F, ellipsis.data(), ellipsis.data() + ellipsis.size()).x;
            const char *visibleEnd = text.data();
            if (availableWidth > ellipsisWidth) {
                static_cast<void>(font.CalcTextSizeA(fontSize, availableWidth - ellipsisWidth, 0.0F, text.data(), text.data() + text.size(),
                                                     &visibleEnd));
                drawList.AddText(&font, fontSize, textPosition, color, text.data(), visibleEnd);
            }
            const float visibleWidth = font.CalcTextSizeA(fontSize, 100000.0F, 0.0F, text.data(), visibleEnd).x;
            drawList.AddText(&font, fontSize, {minimum.x + visibleWidth, textPosition.y}, color, ellipsis.data(),
                             ellipsis.data() + ellipsis.size());
            drawList.PopClipRect();
            return true;
        }

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

        struct AssetDropRequest {
            std::optional<SceneObjectId> parent;
            AssetSceneDropTarget target;
            ImVec2 minimum;
            ImVec2 maximum;
            DocumentRevision revision;
            EditorWorkspaceViewCommandData &command;
            ImDrawList &drawList;
            HierarchyAssetDropZone zone{HierarchyAssetDropZone::Child};
        };

        [[nodiscard]] bool AcceptAssetDrop(const AssetDropRequest &request) {
            if (!ImGui::BeginDragDropTarget())
                return false;
            bool delivered = false;
            const ImGuiPayload *accepted =
                ImGui::AcceptDragDropPayload(AssetSceneDragPayloadType,
                                             ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (const std::optional<AssetSceneDragPayload> payload = ReadAssetPayload(accepted); payload.has_value()) {
                const AssetSceneDropPolicyResult policy = EvaluateAssetSceneDrop(*payload);
                const ImU32 targetColor = Theme::U32(policy.canInstantiate ? Theme::Accent() : Theme::Err());
                if (request.zone == HierarchyAssetDropZone::Child) {
                    request.drawList.AddRect(request.minimum, request.maximum, targetColor, Theme::Layout::Radius, 0, 2.0F);
                } else {
                    const float y = request.zone == HierarchyAssetDropZone::BeforeSibling ? request.minimum.y : request.maximum.y;
                    request.drawList.AddLine({request.minimum.x, y}, {request.maximum.x, y}, targetColor, 3.0F);
                    request.drawList.AddCircleFilled({request.minimum.x + 2.0F, y}, 3.0F, targetColor);
                }
                if (policy.canInstantiate && accepted->IsDelivery()) {
                    delivered = true;
                    request.command.command = EditorWorkspaceViewCommand::InstantiateAsset;
                    request.command.assetSceneDrop = AssetSceneDropRequest{
                        .assetId = payload->assetId.data(),
                        .assetType = payload->assetType.data(),
                        .absoluteAssetPath = payload->absolutePath.data(),
                        .parent = request.parent,
                        .target = request.target,
                        .documentRevision = request.revision,
                    };
                }
            }
            ImGui::EndDragDropTarget();
            return delivered;
        }

        void DrawCreateMenuItems(const std::vector<EditorMenuItem> &items, const std::optional<SceneObjectId> parent,
                                 EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            for (const EditorMenuItem &item : items) {
                const std::string &label = context.localization.Get("editor", item.labelKey);
                const std::string stableLabel = label + "###hierarchy_create_" + std::string(item.labelKey);
                if (item.kind == EditorMenuItemKind::Submenu) {
                    if (Ui::BeginContextSubmenu(stableLabel.c_str(), context.theme.fonts)) {
                        DrawCreateMenuItems(item.children, parent, command, context);
                        Ui::EndContextSubmenu();
                    }
                    continue;
                }
                if (item.kind == EditorMenuItemKind::Command && item.action == EditorMenuAction::CreatePrimitive &&
                    item.primitive.has_value() && Ui::ContextMenuItem(stableLabel.c_str(), nullptr, context.theme.fonts)) {
                    command = HierarchyEditSession::CreateCommand(*item.primitive, parent);
                }
            }
        }
    }  // namespace

    struct HierarchyPanel::PanelInteractionState {
        bool searchActive{false};
        bool panelFocused{false};
        bool workspaceEligible{false};
    };

    struct HierarchyRowGeometry {
        HierarchyRowLayout layout;
        ImVec2 rowMin{};
        ImVec2 rowMax{};
        ImVec2 chevronMin{};
        ImVec2 chevronMax{};
        ImVec2 typeIconMin{};
        ImVec2 typeIconMax{};
        ImVec2 labelMin{};
        ImVec2 labelMax{};
        ImVec2 actionsMin{};
        ImVec2 actionsMax{};
        ImVec2 visibilityMin{};
        ImVec2 visibilityMax{};
        ImVec2 lockMin{};
        ImVec2 lockMax{};
        ImVec2 nextRowCursor{};
    };

    struct HierarchyRowFrameState {
        HierarchyRowGeometry geometry;
        float uiScale{1.0F};
        float nameFontSize{0.0F};
        bool rowHovered{false};
        bool rowFocused{false};
        bool rowLeftClicked{false};
        bool rowRightClicked{false};
        bool selected{false};
        bool pointerInActions{false};
        bool assetDropDelivered{false};
        bool searching{false};
    };

    struct HierarchyPanel::RowFrame {
        RowFrame(const HierarchyVisibleRow &row, const HierarchyNode &node, ImDrawList &drawList, ImFont &nameFont,
                 const HierarchyRowFrameState &state)
            : row(row), node(node), drawList(drawList), nameFont(nameFont), geometry(state.geometry), uiScale(state.uiScale),
              nameFontSize(state.nameFontSize), rowHovered(state.rowHovered), rowFocused(state.rowFocused),
              rowLeftClicked(state.rowLeftClicked), rowRightClicked(state.rowRightClicked), selected(state.selected),
              pointerInActions(state.pointerInActions), assetDropDelivered(state.assetDropDelivered), searching(state.searching) {}

        const HierarchyVisibleRow &row;
        const HierarchyNode &node;
        ImDrawList &drawList;
        ImFont &nameFont;
        HierarchyRowGeometry geometry;
        float uiScale{1.0F};
        float nameFontSize{0.0F};
        bool rowHovered{false};
        bool rowFocused{false};
        bool rowLeftClicked{false};
        bool rowRightClicked{false};
        bool selected{false};
        bool pointerInActions{false};
        bool assetDropDelivered{false};
        bool searching{false};
    };

    struct HierarchyPanel::RowControls {
        bool chevronHovered{false};
        bool chevronPressed{false};
        bool visibilityHovered{false};
        bool visibilityPressed{false};
        bool lockHovered{false};
        bool lockPressed{false};

        [[nodiscard]] bool IsHovered(const RowFrame &frame) const noexcept {
            return frame.rowHovered || chevronHovered || visibilityHovered || lockHovered;
        }
    };

    struct HierarchyPanel::RowActionIcon {
        Ui::UiIcon icon{Ui::UiIcon::Visibility};
        ImVec2 minimum{};
        ImVec2 maximum{};
        bool hovered{false};
        bool active{false};
        bool inherited{false};
    };

    namespace {
        [[nodiscard]] HierarchyRowGeometry BuildRowGeometry(const ImVec2 rowMin, const float listWidth, const std::uint32_t depth,
                                                            const float uiScale) {
            const HierarchyRowLayout rowLayout = CalculateHierarchyRowLayout(listWidth, depth, uiScale, kRowActionsWidth * uiScale);
            const ImVec2 rowMax{rowMin.x + listWidth, rowMin.y + rowLayout.height};
            const ImVec2 actionsMin{rowMin.x + rowLayout.actions.minimum, rowMin.y};
            const ImVec2 actionsMax{rowMin.x + rowLayout.actions.maximum, rowMax.y};
            return {
                .layout = rowLayout,
                .rowMin = rowMin,
                .rowMax = rowMax,
                .chevronMin = {rowMin.x + rowLayout.chevron.minimum, rowMin.y},
                .chevronMax = {rowMin.x + rowLayout.chevron.maximum, rowMax.y},
                .typeIconMin = {rowMin.x + rowLayout.typeIcon.minimum, rowMin.y},
                .typeIconMax = {rowMin.x + rowLayout.typeIcon.maximum, rowMax.y},
                .labelMin = {rowMin.x + rowLayout.label.minimum, rowMin.y},
                .labelMax = {rowMin.x + rowLayout.label.maximum, rowMax.y},
                .actionsMin = actionsMin,
                .actionsMax = actionsMax,
                .visibilityMin = {rowMin.x + rowLayout.visibilityAction.minimum, rowMin.y},
                .visibilityMax = {rowMin.x + rowLayout.visibilityAction.maximum, rowMax.y},
                .lockMin = {rowMin.x + rowLayout.lockAction.minimum, rowMin.y},
                .lockMax = {rowMin.x + rowLayout.lockAction.maximum, rowMax.y},
                .nextRowCursor = ImVec2{rowMin.x, rowMax.y},
            };
        }
    }  // namespace

    void HierarchyPanel::OnAttach(PanelContext &context) {
        inputRouter_ = context.inputRouter;
        workspaceInputContext_ = context.workspaceInputContext;
    }

    void HierarchyPanel::OnDetach() {
        focusedWidgetContext_.Reset();
        inputRouter_ = nullptr;
        workspaceInputContext_ = nullptr;
    }

    void HierarchyPanel::BeginRename(const HierarchyNodeId id) {
        const HierarchyNode *node = editSession_.Find(id);
        if (node == nullptr) {
            return;
        }
        const auto result = std::format_to_n(renameBuffer_.data(), renameBuffer_.size() - 1U, "{}", node->name);
        *result.out = '\0';
        renamingId_ = id;
        requestRenameFocus_ = true;
    }

    void HierarchyPanel::DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, const ImU32 color) {
        constexpr float iconSize = 16.0F;
        const float x = pos.x + (size.x - iconSize) * 0.5F;
        const float y = pos.y + (size.y - iconSize) * 0.5F;
        dl->AddRect(ImVec2(x + 2.0F, y + 3.0F), ImVec2(x + 14.0F, y + 13.0F), color, 1.0F, 0, 1.4F);
        dl->AddLine(ImVec2(x + 6.0F, y + 3.0F), ImVec2(x + 6.0F, y + 13.0F), color, 1.4F);
    }

    HierarchyPanel::PanelInteractionState HierarchyPanel::DrawSearch(const float panelWidth, const float uiScale,
                                                                     const EditorGuiContext &context) {
        const ImVec2 windowPosition = ImGui::GetWindowPos();
        ImDrawList &drawList = *ImGui::GetWindowDrawList();
        const float toolbarHeight = kToolbarHeight * uiScale;
        const float searchRegionHeight = kSearchRegionHeight * uiScale;
        const ImVec2 regionMinimum{windowPosition.x, windowPosition.y + toolbarHeight};
        const ImVec2 regionMaximum{windowPosition.x + panelWidth, regionMinimum.y + searchRegionHeight};

        ImGui::SetCursorPos(ImVec2(0.0F, toolbarHeight));
        ImGui::SetNextItemWidth(std::max(1.0F, panelWidth));
        const float verticalPadding = std::max(0.0F, (searchRegionHeight - Theme::TextPx::Label()) * 0.5F);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.0F * uiScale, verticalPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
        bool searchActive = false;
        {
            Theme::ScopedTextStyle searchFont(context.theme.fonts.sans, Theme::TextPx::Label(), Theme::FontPx::Sans);
            if (requestSearchFocus_) {
                ImGui::SetKeyboardFocusHere();
                requestSearchFocus_ = false;
            }
            ImGui::InputTextWithHint("##HierarchySearch", context.localization.Get("editor", "workspace.hierarchy.search").c_str(),
                                     searchBuffer_.data(), searchBuffer_.size());
            searchActive = ImGui::IsItemActive();
        }
        const ImVec2 inputMaximum = ImGui::GetItemRectMax();
        const ImVec2 searchCenter{inputMaximum.x - 18.0F * uiScale, (ImGui::GetItemRectMin().y + inputMaximum.y) * 0.5F};
        const ImU32 searchColor = Theme::U32(Theme::Muted());
        drawList.AddCircle({searchCenter.x - 1.5F * uiScale, searchCenter.y - 1.5F * uiScale}, 4.2F * uiScale, searchColor, 16,
                           1.4F * uiScale);
        drawList.AddLine({searchCenter.x + 1.5F * uiScale, searchCenter.y + 1.5F * uiScale},
                         {searchCenter.x + 5.0F * uiScale, searchCenter.y + 5.0F * uiScale}, searchColor, 1.4F * uiScale);
        drawList.AddLine({regionMinimum.x, regionMaximum.y - uiScale}, {regionMaximum.x, regionMaximum.y - uiScale},
                         Theme::U32(Theme::Border()), uiScale);
        const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        const bool workspaceEligible =
            inputRouter_ != nullptr && workspaceInputContext_ != nullptr && inputRouter_->IsContextActive(*workspaceInputContext_);
        return {.searchActive = searchActive, .panelFocused = panelFocused, .workspaceEligible = workspaceEligible};
    }

    void HierarchyPanel::UpdateFocusedInputContext(const bool searchActive) {
        const bool needsFocusedContext = searchActive || renamingId_.has_value();
        if (needsFocusedContext && !focusedWidgetContext_.IsActive() && inputRouter_ != nullptr)
            focusedWidgetContext_ =
                inputRouter_->PushContext(Input::InputContextId{"editor.hierarchy.text"}, Input::InputContextKind::FocusedGuiWidget);
        else if (!needsFocusedContext)
            focusedWidgetContext_.Reset();
    }

    void HierarchyPanel::HandleRenameShortcut(const PanelInteractionState &interaction) {
        if (!interaction.workspaceEligible || !interaction.panelFocused || interaction.searchActive || renamingId_.has_value() ||
            !editSession_.SelectedId().has_value() || !inputRouter_->ConsumeKey(*workspaceInputContext_, Input::Key::F2))
            return;
        const HierarchyNode *selectedNode = editSession_.Find(*editSession_.SelectedId());
        if (selectedNode != nullptr && !selectedNode->effectivelyLocked)
            BeginRename(*editSession_.SelectedId());
    }

    void HierarchyPanel::DrawRowContextMenu(const RowFrame &frame, const bool workspaceEligible, bool &pendingDelete,
                                            EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (frame.pointerInActions || !Ui::BeginContextMenu("##HierarchyContext"))
            return;
        const bool editable = workspaceEligible && !frame.node.effectivelyLocked;
        if (editable &&
            Ui::BeginContextSubmenu((context.localization.Get("editor", "workspace.create") + "###hierarchy_create_root").c_str(),
                                    context.theme.fonts)) {
            DrawCreateMenuItems(GetPrimitiveCreateMenuItems(), SceneObjectId{frame.node.id}, command, context);
            Ui::EndContextSubmenu();
        }
        Ui::ContextMenuSeparator();
        if (editable &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.hierarchy.rename") + "###hierarchy_action_rename").c_str(),
                                "F2", context.theme.fonts))
            BeginRename(frame.node.id);
        if (editable &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.hierarchy.duplicate") + "###hierarchy_action_duplicate")
                                    .c_str(),
                                nullptr, context.theme.fonts))
            command = HierarchyEditSession::DuplicateCommand(frame.node.id);
        Ui::ContextMenuSeparator();
        if (editable &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.hierarchy.delete") + "###hierarchy_action_delete").c_str(),
                                "Delete", context.theme.fonts, Ui::ContextMenuItemTone::Danger))
            pendingDelete = true;
        Ui::EndContextMenu();
    }

    HierarchyPanel::RowControls HierarchyPanel::DrawRowControls(const RowFrame &frame, const bool workspaceEligible,
                                                                const EditorGuiContext &context) {
        RowControls controls;
        if (!frame.node.children.empty() && searchBuffer_[0] == '\0') {
            ImGui::SetCursorScreenPos(frame.geometry.chevronMin);
            ImGui::InvisibleButton("##hierarchy_chevron", ImVec2(frame.geometry.layout.chevron.Width(), frame.geometry.layout.height));
            controls.chevronHovered = ImGui::IsItemHovered();
            controls.chevronPressed = workspaceEligible && ImGui::IsItemClicked(ImGuiMouseButton_Left);
            ImGui::SetCursorScreenPos(frame.geometry.nextRowCursor);
        }
        if (frame.geometry.layout.visibilityAction.Width() <= 0.0F)
            return controls;

        ImGui::SetCursorScreenPos(frame.geometry.visibilityMin);
        ImGui::InvisibleButton("##hierarchy_visibility",
                               ImVec2(frame.geometry.layout.visibilityAction.Width(), frame.geometry.layout.height));
        controls.visibilityHovered = ImGui::IsItemHovered();
        controls.visibilityPressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (controls.visibilityHovered) {
            const char *tooltip = "workspace.hierarchy.show";
            if (frame.node.hiddenByParent && frame.node.locallyVisible)
                tooltip = "workspace.hierarchy.hidden_by_parent";
            else if (frame.node.locallyVisible)
                tooltip = "workspace.hierarchy.hide";
            Ui::ShowTooltip(context.localization.Get("editor", tooltip).c_str(), &context.theme.fonts);
        }

        ImGui::SetCursorScreenPos(frame.geometry.lockMin);
        ImGui::InvisibleButton("##hierarchy_lock", ImVec2(frame.geometry.layout.lockAction.Width(), frame.geometry.layout.height));
        controls.lockHovered = ImGui::IsItemHovered();
        controls.lockPressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (controls.lockHovered) {
            const char *tooltip = "workspace.hierarchy.lock";
            if (frame.node.lockedByParent && !frame.node.locallyLocked)
                tooltip = "workspace.hierarchy.locked_by_parent";
            else if (frame.node.locallyLocked)
                tooltip = "workspace.hierarchy.unlock";
            Ui::ShowTooltip(context.localization.Get("editor", tooltip).c_str(), &context.theme.fonts);
        }
        ImGui::SetCursorScreenPos(frame.geometry.nextRowCursor);
        return controls;
    }

    void HierarchyPanel::DrawRowBackground(const RowFrame &frame, const bool hovered) {
        if (frame.selected) {
            ImVec4 accent = Theme::Accent();
            accent.w = hovered ? 0.16F : 0.14F;
            frame.drawList.AddRectFilled(frame.geometry.rowMin, frame.geometry.rowMax, Theme::U32(accent), 3.0F * frame.uiScale);
        } else if (hovered) {
            frame.drawList.AddRectFilled(frame.geometry.rowMin, frame.geometry.rowMax, Theme::U32(Theme::Hover()), 3.0F * frame.uiScale);
        }
        if (frame.rowFocused)
            frame.drawList.AddRect(frame.geometry.rowMin, frame.geometry.rowMax, Theme::U32(Theme::BorderStrong()), 3.0F * frame.uiScale, 0,
                                   frame.uiScale);
    }

    void HierarchyPanel::DrawRowTree(const RowFrame &frame, const RowControls &controls, const float centerY) {
        const float chevronCenterX = (frame.geometry.chevronMin.x + frame.geometry.chevronMax.x) * 0.5F;
        if (!frame.node.children.empty()) {
            if (frame.node.expanded || frame.searching)
                frame.drawList.AddTriangleFilled({chevronCenterX - 3.0F * frame.uiScale, centerY - 2.0F * frame.uiScale},
                                                 {chevronCenterX + 3.0F * frame.uiScale, centerY - 2.0F * frame.uiScale},
                                                 {chevronCenterX, centerY + 2.0F * frame.uiScale},
                                                 Theme::U32(controls.chevronHovered ? Theme::Text() : Theme::Dim()));
            else
                frame.drawList.AddTriangleFilled({chevronCenterX - 2.0F * frame.uiScale, centerY - 3.0F * frame.uiScale},
                                                 {chevronCenterX - 2.0F * frame.uiScale, centerY + 3.0F * frame.uiScale},
                                                 {chevronCenterX + 2.0F * frame.uiScale, centerY},
                                                 Theme::U32(controls.chevronHovered ? Theme::Text() : Theme::Dim()));
        }
    }

    void HierarchyPanel::DrawRowTypeIcon(const RowFrame &frame, const EditorGuiContext &context, const float centerY) {
        const HierarchyIconPresentation icon = GetIconPresentation(frame.node.type);
        const float iconSize = 16.0F * frame.uiScale;
        ImVec4 typeColor = frame.selected ? Theme::Accent() : icon.color;
        if (frame.node.effectivelyLocked)
            typeColor.w *= 0.65F;
        Ui::DrawEditorIcon(&frame.drawList, icon.icon, {frame.geometry.typeIconMin.x, centerY - iconSize * 0.5F}, {iconSize, iconSize},
                           Theme::U32(typeColor), context.theme.fonts.icon);
        if (icon.tooltipKey != nullptr && ImGui::IsMouseHoveringRect(frame.geometry.typeIconMin, frame.geometry.typeIconMax))
            Ui::ShowTooltip(context.localization.Get("editor", icon.tooltipKey).c_str(), &context.theme.fonts);
    }

    void HierarchyPanel::DrawRowActionIcon(const RowFrame &frame, const float iconSize, const RowActionIcon &action) {
        if (iconSize <= 0.0F)
            return;
        ImVec4 color = action.hovered || action.active ? Theme::Text() : Theme::Muted();
        if (action.active && !action.hovered)
            color.w *= 0.82F;
        if (action.inherited)
            color.w *= 0.55F;
        const ImVec2 position{action.minimum.x + ((action.maximum.x - action.minimum.x) - iconSize) * 0.5F,
                              action.minimum.y + ((action.maximum.y - action.minimum.y) - iconSize) * 0.5F};
        Ui::DrawEditorIcon(&frame.drawList, action.icon, position, {iconSize, iconSize}, Theme::U32(color));
    }

    void HierarchyPanel::DrawRowActions(const RowFrame &frame, const RowControls &controls) {
        if (frame.geometry.layout.visibilityAction.Width() <= 0.0F || (!frame.selected && !controls.IsHovered(frame)))
            return;
        const float actionIconSize =
            std::max(0.0F, std::min({15.0F * frame.uiScale, frame.geometry.layout.visibilityAction.Width() - 4.0F * frame.uiScale,
                                     frame.geometry.layout.lockAction.Width() - 4.0F * frame.uiScale,
                                     frame.geometry.layout.height - 4.0F * frame.uiScale}));
        DrawRowActionIcon(frame, actionIconSize,
                          {
                              .icon = frame.node.effectivelyVisible ? Ui::UiIcon::Visibility : Ui::UiIcon::VisibilityOff,
                              .minimum = frame.geometry.visibilityMin,
                              .maximum = frame.geometry.visibilityMax,
                              .hovered = controls.visibilityHovered,
                              .active = !frame.node.effectivelyVisible,
                              .inherited = frame.node.hiddenByParent && frame.node.locallyVisible,
                          });
        DrawRowActionIcon(frame, actionIconSize,
                          {
                              .icon = Ui::UiIcon::Lock,
                              .minimum = frame.geometry.lockMin,
                              .maximum = frame.geometry.lockMax,
                              .hovered = controls.lockHovered,
                              .active = frame.node.effectivelyLocked,
                              .inherited = frame.node.lockedByParent && !frame.node.locallyLocked,
                          });
    }

    void HierarchyPanel::DrawRowPresentation(const RowFrame &frame, const RowControls &controls, const EditorGuiContext &context) {
        DrawRowBackground(frame, controls.IsHovered(frame));
        const float centerY = frame.geometry.rowMin.y + frame.geometry.layout.height * 0.5F;
        DrawRowTree(frame, controls, centerY);
        DrawRowTypeIcon(frame, context, centerY);
        DrawRowActions(frame, controls);
    }

    void HierarchyPanel::ApplyRowInteraction(const RowFrame &frame, const RowControls &controls, const bool workspaceEligible,
                                             EditorWorkspaceViewCommandData &command) {
        if (frame.assetDropDelivered)
            return;
        if (controls.chevronPressed) {
            editSession_.ToggleExpanded(frame.node.id);
            return;
        }
        if (workspaceEligible && controls.visibilityPressed) {
            if (!(frame.node.hiddenByParent && frame.node.locallyVisible))
                command = HierarchyEditSession::ToggleVisibilityCommand(frame.node);
            return;
        }
        if (workspaceEligible && controls.lockPressed) {
            if (!(frame.node.lockedByParent && !frame.node.locallyLocked))
                command = HierarchyEditSession::ToggleLockCommand(frame.node);
            return;
        }
        if (!workspaceEligible || !frame.rowLeftClicked || frame.pointerInActions)
            return;
        editSession_.Select(frame.node.id);
        const ImGuiIO &io = ImGui::GetIO();
        HierarchySelectionGesture gesture = HierarchySelectionGesture::Replace;
        if (io.KeyShift)
            gesture = HierarchySelectionGesture::Range;
        else if (io.KeyCtrl || io.KeySuper)
            gesture = HierarchySelectionGesture::Toggle;
        command = editSession_.SelectCommand(frame.node.id, gesture);
    }

    void HierarchyPanel::DrawRowLabel(const RowFrame &frame, EditorWorkspaceViewCommandData &command) {
        const float centerY = frame.geometry.rowMin.y + frame.geometry.layout.height * 0.5F;
        if (renamingId_ != frame.node.id) {
            ImVec4 labelColor = frame.node.children.empty() ? BlendColor(Theme::Muted(), Theme::Text(), 0.36F)
                                                            : BlendColor(Theme::Muted(), Theme::Text(), 0.68F);
            if (frame.selected)
                labelColor = Theme::Text();
            if (frame.node.effectivelyLocked)
                labelColor = Theme::Muted();
            if (const bool truncated = DrawHierarchyLabel({.drawList = frame.drawList,
                                                           .font = frame.nameFont,
                                                           .fontSize = frame.nameFontSize,
                                                           .minimum = frame.geometry.labelMin,
                                                           .maximum = frame.geometry.labelMax,
                                                           .centerY = centerY,
                                                           .color = Theme::U32(labelColor),
                                                           .text = frame.node.name});
                truncated && ImGui::IsMouseHoveringRect(frame.geometry.labelMin, frame.geometry.labelMax))
                Ui::ShowTooltip(frame.node.name.c_str());
            return;
        }

        ImGui::SetCursorScreenPos({frame.geometry.labelMin.x, frame.geometry.rowMin.y + 2.0F * frame.uiScale});
        ImGui::SetNextItemWidth(std::max(1.0F, frame.geometry.labelMax.x - frame.geometry.labelMin.x));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3.0F * frame.uiScale, 1.0F * frame.uiScale});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Accent());
        if (requestRenameFocus_) {
            ImGui::SetKeyboardFocusHere();
            requestRenameFocus_ = false;
        }
        bool submittedByWidget = false;
        {
            Theme::ScopedTextStyle renameFont(&frame.nameFont, frame.nameFontSize, Theme::FontPx::Sans);
            submittedByWidget = ImGui::InputText("##Rename", renameBuffer_.data(), renameBuffer_.size(),
                                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        }
        const bool submitted =
            submittedByWidget && inputRouter_ != nullptr && inputRouter_->ConsumeKey(focusedWidgetContext_, Input::Key::Enter);
        const bool cancelled = inputRouter_ != nullptr && inputRouter_->ConsumeKey(focusedWidgetContext_, Input::Key::Escape);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (submitted) {
            command = HierarchyEditSession::RenameCommand(frame.node.id, renameBuffer_.data());
            renamingId_.reset();
        } else if (cancelled) {
            renamingId_.reset();
        }
    }

    bool HierarchyPanel::AcceptRowAssetDrop(const HierarchyNodeId nodeId, const float normalizedRowY, const ImVec2 &rowMin,
                                            const ImVec2 &rowMax, const EditorWorkspaceViewModel &viewModel,
                                            EditorWorkspaceViewCommandData &command, ImDrawList &drawList) {
        const std::optional<HierarchyNodeId> projectedParent = editSession_.ParentId(nodeId);
        const std::optional<SceneObjectId> nodeParent =
            projectedParent.has_value() ? std::optional{SceneObjectId{*projectedParent}} : std::nullopt;
        const HierarchyAssetDropPlacement assetPlacement =
            ResolveHierarchyAssetDropPlacement(normalizedRowY, SceneObjectId{nodeId}, nodeParent);
        return AcceptAssetDrop({.parent = assetPlacement.parent,
                                .target = assetPlacement.target,
                                .minimum = rowMin,
                                .maximum = rowMax,
                                .revision = viewModel.documentRevision,
                                .command = command,
                                .drawList = drawList,
                                .zone = assetPlacement.zone});
    }

    HierarchyPanel::RowFrame HierarchyPanel::BuildRowFrame(const HierarchyVisibleRow &row, const RowDrawLayout &drawLayout,
                                                           const EditorWorkspaceViewModel &viewModel,
                                                           EditorWorkspaceViewCommandData &command, ImDrawList &drawList,
                                                           const EditorGuiContext &context) {
        const HierarchyNode &node = *row.node;
        ImFont &nameFont = *ResolveFont(node.children.empty() ? context.theme.fonts.sans : context.theme.fonts.sansEmphasis);
        ImGui::PushID(&node.id);
        ImGui::SetCursorPosX(drawLayout.outerPadding);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const HierarchyRowGeometry geometry = BuildRowGeometry(rowMin, drawLayout.listWidth, row.depth, drawLayout.uiScale);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##hierarchy_object_row", {drawLayout.listWidth, geometry.layout.height});
        const ImVec2 nextRowCursor = ImGui::GetCursorScreenPos();
        const bool rowHovered = ImGui::IsItemHovered();
        const bool rowFocused = ImGui::IsItemFocused();
        const bool rowLeftClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool rowRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        const bool pointerInActions = ImGui::IsMouseHoveringRect(geometry.actionsMin, geometry.actionsMax);
        const float normalizedRowY = std::clamp((ImGui::GetMousePos().y - geometry.rowMin.y) / geometry.layout.height, 0.0F, 1.0F);
        const bool assetDropDelivered =
            AcceptRowAssetDrop(node.id, normalizedRowY, geometry.rowMin, geometry.rowMax, viewModel, command, drawList);
        HierarchyRowGeometry frameGeometry = geometry;
        frameGeometry.nextRowCursor = nextRowCursor;
        return RowFrame{row,
                        node,
                        drawList,
                        nameFont,
                        {.geometry = frameGeometry,
                         .uiScale = drawLayout.uiScale,
                         .nameFontSize = Theme::TextPx::Label(),
                         .rowHovered = rowHovered,
                         .rowFocused = rowFocused,
                         .rowLeftClicked = rowLeftClicked,
                         .rowRightClicked = rowRightClicked,
                         .selected = editSession_.IsSelected(node.id),
                         .pointerInActions = pointerInActions,
                         .assetDropDelivered = assetDropDelivered,
                         .searching = searchBuffer_[0] != '\0'}};
    }

    bool HierarchyPanel::DrawRows(const std::vector<HierarchyVisibleRow> &rows, const RowDrawLayout &layout,
                                  const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                  const EditorGuiContext &context) {
        bool pendingDelete = false;
        ImDrawList &drawList = *ImGui::GetWindowDrawList();
        const bool workspaceEligible =
            inputRouter_ != nullptr && workspaceInputContext_ != nullptr && inputRouter_->IsContextActive(*workspaceInputContext_);

        for (const HierarchyVisibleRow &row : rows) {
            const RowFrame frame = BuildRowFrame(row, layout, viewModel, command, drawList, context);
            if (workspaceEligible && frame.rowRightClicked && !frame.pointerInActions && !frame.selected) {
                editSession_.Select(frame.node.id);
                command = editSession_.SelectCommand(frame.node.id, HierarchySelectionGesture::Replace);
            }
            DrawRowContextMenu(frame, workspaceEligible, pendingDelete, command, context);
            const RowControls controls = DrawRowControls(frame, workspaceEligible, context);
            DrawRowPresentation(frame, controls, context);
            ApplyRowInteraction(frame, controls, workspaceEligible, command);
            DrawRowLabel(frame, command);
            ImGui::SetCursorScreenPos(frame.geometry.nextRowCursor);
            ImGui::PopID();
        }

        if (rows.empty()) {
            ImGui::SetCursorPosX(outerPadding + 8.0F * uiScale);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
            ImGui::TextUnformatted(
                context.localization
                    .Get("editor", searchBuffer_[0] == '\0' ? "workspace.hierarchy.empty" : "workspace.hierarchy.no_matches")
                    .c_str());
            ImGui::PopStyleColor();
        }
        return pendingDelete;
    }

    void HierarchyPanel::DrawPanel(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm,
                                   EditorWorkspaceViewCommandData &cmd, const EditorGuiContext &ctx) {
        static_cast<void>(pos);
        editSession_.Synchronize(vm);
        const float uiScale = Theme::GetActiveTokens().sizes.uiScale;
        const float tabHeight = kTabHeight * uiScale;
        const float toolbarHeight = kToolbarHeight * uiScale;
        DrawHierarchyTab(ctx.localization.Get("editor", "workspace.panel.hierarchy").c_str(), ctx.theme.fonts, uiScale);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::BeginChild("##HierarchyContent", ImVec2(size.x, size.y - tabHeight), false, ImGuiWindowFlags_NoSavedSettings);

        const HierarchyToolbarResult toolbar = DrawHierarchyToolbar(size.x, ctx, uiScale);
        if (toolbar.createPressed)
            ImGui::OpenPopup("##HierarchyCreatePopup");
        if (toolbar.focusSearchPressed)
            requestSearchFocus_ = true;
        if (Ui::BeginMenuPopup("##HierarchyCreatePopup")) {
            DrawCreateMenuItems(GetPrimitiveCreateMenuItems(), std::nullopt, cmd, ctx);
            Ui::EndMenuPopup();
        }

        const PanelInteractionState interaction = DrawSearch(size.x, uiScale, ctx);
        UpdateFocusedInputContext(interaction.searchActive);
        const std::vector<HierarchyVisibleRow> &visibleRows = editSession_.VisibleRows(searchBuffer_.data());
        HandleRenameShortcut(interaction);

        const float outerPadding = kOuterPadding * uiScale;
        const float contentHeight = std::max(1.0F, size.y - tabHeight);
        const float scrollTop = toolbarHeight + kSearchRegionHeight * uiScale;
        const float scrollHeight = std::max(1.0F, contentHeight - scrollTop - kFooterHeight * uiScale);
        ImGui::SetCursorPos({0.0F, scrollTop});
        ImGui::BeginChild("##HierarchyScroll", {size.x, scrollHeight}, false, ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorPosY(5.0F * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, 0.0F));
        const float listWidth = std::max(1.0F, size.x - outerPadding * 2.0F);
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        bool pendingDelete =
            DrawRows(visibleRows, RowDrawLayout{.listWidth = listWidth, .outerPadding = outerPadding, .uiScale = uiScale}, vm, cmd, ctx);

        const ImVec2 remaining = ImGui::GetContentRegionAvail();
        const ImVec2 rootDropMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##HierarchyRootDrop", ImVec2(std::max(1.0F, remaining.x), std::max(32.0F, remaining.y)));
        const ImVec2 rootDropMax{rootDropMin.x + std::max(1.0F, remaining.x), rootDropMin.y + std::max(32.0F, remaining.y)};
        static_cast<void>(AcceptAssetDrop({.parent = std::nullopt,
                                           .target = AssetSceneDropTarget::HierarchyRoot,
                                           .minimum = rootDropMin,
                                           .maximum = rootDropMax,
                                           .revision = vm.documentRevision,
                                           .command = cmd,
                                           .drawList = *drawList}));
        if (remaining.y >= 64.0F * uiScale) {
            const ImVec2 zoneMin{rootDropMin.x + 12.0F * uiScale, rootDropMax.y - 64.0F * uiScale};
            const ImVec2 zoneMax{rootDropMax.x - 12.0F * uiScale, rootDropMax.y - 10.0F * uiScale};
            DrawDashedRect(*drawList, zoneMin, zoneMax, Theme::U32(Theme::BorderStrong()), uiScale);
            ImFont *font = ResolveFont(ctx.theme.fonts.sans);
            const std::string &label = ctx.localization.Get("editor", "workspace.hierarchy.drop_to_root");
            const float fontSize = Theme::TextPx::Caption();
            const ImVec2 labelSize = font->CalcTextSizeA(fontSize, 100000.0F, 0.0F, label.c_str());
            drawList->AddText(font, fontSize,
                              {zoneMin.x + (zoneMax.x - zoneMin.x - labelSize.x) * 0.5F,
                               zoneMin.y + (zoneMax.y - zoneMin.y - labelSize.y) * 0.5F},
                              Theme::U32(Theme::Dim()), label.c_str());
        }
        if (Ui::BeginContextMenu("##HierarchyRootContext")) {
            if (interaction.workspaceEligible &&
                Ui::BeginContextSubmenu((ctx.localization.Get("editor", "workspace.create") + "###hierarchy_create_root").c_str(),
                                        ctx.theme.fonts)) {
                DrawCreateMenuItems(GetPrimitiveCreateMenuItems(), std::nullopt, cmd, ctx);
                Ui::EndContextSubmenu();
            }
            Ui::EndContextMenu();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();

        ImGui::SetCursorPos({0.0F, contentHeight - kFooterHeight * uiScale});
        const ImVec2 footerMin = ImGui::GetCursorScreenPos();
        ImDrawList &footerDrawList = *ImGui::GetWindowDrawList();
        footerDrawList.AddRectFilled(footerMin, {footerMin.x + size.x, footerMin.y + kFooterHeight * uiScale}, Theme::U32(Theme::Bg0()));
        footerDrawList.AddLine(footerMin, {footerMin.x + size.x, footerMin.y}, Theme::U32(Theme::Border()));
        ImFont *footerFont = ResolveFont(ctx.theme.fonts.sans);
        const float footerFontSize = Theme::TextPx::Caption();
        const std::string objectCount =
            FormatObjectCount(ctx.localization.Get("editor", "workspace.hierarchy.footer.objects"), vm.objects.size());
        const std::string &footerLabel = ctx.localization.Get("editor", "workspace.hierarchy.footer.label");
        const ImVec2 footerLabelSize = footerFont->CalcTextSizeA(footerFontSize, 100000.0F, 0.0F, footerLabel.c_str());
        const float footerTextY = footerMin.y + (kFooterHeight * uiScale - footerLabelSize.y) * 0.5F;
        footerDrawList.AddText(footerFont, footerFontSize, {footerMin.x + 10.0F * uiScale, footerTextY}, Theme::U32(Theme::Dim()),
                               objectCount.c_str());
        footerDrawList.AddText(footerFont, footerFontSize, {footerMin.x + size.x - 10.0F * uiScale - footerLabelSize.x, footerTextY},
                               Theme::U32(Theme::Dim()), footerLabel.c_str());
        ImGui::Dummy({size.x, kFooterHeight * uiScale});

        if (interaction.workspaceEligible && interaction.panelFocused && !interaction.searchActive && !renamingId_.has_value() &&
            editSession_.SelectedId().has_value()) {
            using enum Input::Key;
            const Input::ModifierState &modifiers = inputRouter_->Snapshot().modifiers;
            if ((HierarchyEditSession::IsDeleteShortcut(Delete, modifiers) && inputRouter_->ConsumeKey(*workspaceInputContext_, Delete)) ||
                (HierarchyEditSession::IsDeleteShortcut(Backspace, modifiers) &&
                 inputRouter_->ConsumeKey(*workspaceInputContext_, Backspace))) {
                pendingDelete = true;
            }
        }
        if (pendingDelete) {
            renamingId_.reset();
            cmd = editSession_.DeleteSelectionCommand();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}  // namespace Horo::Editor
