#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserGrid.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/AssetSceneDrop.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserActions.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserCards.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserDialogs.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserToolbar.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr float CardRadius = 6.0F;

        [[nodiscard]] float HeaderFontSize() {
            return Theme::TextPx::Label();
        }

        [[nodiscard]] float CardFontSize() {
            return Theme::TextPx::Label();
        }

        constexpr float PreviewRowHeight = 20.0F;

        [[nodiscard]] ImFont *ResolveFont(ImFont *preferred) {
            return preferred != nullptr ? preferred : ImGui::GetFont();
        }

        [[nodiscard]] std::optional<AssetSceneDragPayload> AssetReferenceFromPayload(const ImGuiPayload *payload) {
            if (payload == nullptr || !payload->IsDataType(AssetSceneDragPayloadType) || payload->Data == nullptr ||
                payload->DataSize != sizeof(AssetSceneDragPayload)) {
                return std::nullopt;
            }
            AssetSceneDragPayload reference;
            std::memcpy(&reference, payload->Data, sizeof(reference));
            if (reference.absolutePath.back() != '\0')
                return std::nullopt;
            return reference;
        }

        [[nodiscard]] std::optional<std::string> AbsoluteAssetPathFromPayload(const ImGuiPayload *payload) {
            const std::optional<AssetSceneDragPayload> reference = AssetReferenceFromPayload(payload);
            if (!reference.has_value())
                return std::nullopt;
            const std::filesystem::path path{reference->absolutePath.data()};
            if (!path.is_absolute())
                return std::nullopt;
            return path.lexically_normal().string();
        }

        void HandleDirectoryDragDropTarget(const ContentBrowserEntry &entry, const ImVec2 &cardMin, const float cardWidth,
                                           const float cardHeight, ImDrawList *drawList, EditorWorkspaceViewCommandData &command) {
            if (entry.kind != ContentBrowserEntryKind::Directory || !ImGui::BeginDragDropTarget()) {
                return;
            }
            const ImGuiPayload *acceptedPayload =
                ImGui::AcceptDragDropPayload(AssetSceneDragPayloadType,
                                             ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (const std::optional<std::string> source = AbsoluteAssetPathFromPayload(acceptedPayload); source.has_value()) {
                const bool copy = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
                const bool validTarget = copy || std::filesystem::path{*source}.parent_path() != std::filesystem::path{entry.absolutePath};
                drawList->AddRect(cardMin, {cardMin.x + cardWidth, cardMin.y + cardHeight},
                                  Theme::U32(validTarget ? Theme::Accent() : Theme::Err()), CardRadius, 0, 2.0F);
                if (validTarget && acceptedPayload->IsDelivery()) {
                    command = AssetBrowserInteractionSession::Transfer(*source, entry.absolutePath,
                                                                       copy ? ContentBrowserTransferMode::Copy
                                                                            : ContentBrowserTransferMode::Move);
                }
            }
            ImGui::EndDragDropTarget();
        }

        void HandleAssetDragDropSource(const ContentBrowserEntry &entry, const ILocalizationService &localization) {
            if (entry.kind != ContentBrowserEntryKind::Asset || !ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                return;
            }
            const AssetSceneDragPayload payload =
                MakeAssetSceneDragPayload(entry.assetId, entry.assetType, entry.absolutePath, entry.registered);
            ImGui::SetDragDropPayload(AssetSceneDragPayloadType, &payload, sizeof(payload));
            ImGui::TextUnformatted(entry.displayName.c_str());
            const bool copy = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
            ImGui::TextColored(Theme::Dim(), "%s",
                               localization
                                   .Get("editor",
                                        copy ? "workspace.content_browser.action.copy" : "workspace.content_browser.action.move_here")
                                   .c_str());
            ImGui::EndDragDropSource();
        }

        void DrawAssetLocationRail(const ImVec2 minimum, const float height, const EditorGuiContext &context) {
            using enum Ui::UiIcon;
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const float width = AssetBrowserLayout::LocationRailWidth;
            drawList->AddRectFilled(minimum, {minimum.x + width, minimum.y + height}, Theme::U32(Theme::Bg1()));
            drawList->AddLine({minimum.x + width - 1.0F, minimum.y}, {minimum.x + width - 1.0F, minimum.y + height},
                              Theme::U32(Theme::Border()));
            const std::array icons{Favorite, History, Storage, Package, AccountTree, Tag};
            const std::array keys{"workspace.content_browser.location.favorites",    "workspace.content_browser.location.recent",
                                  "workspace.content_browser.location.sources",      "workspace.content_browser.location.packages",
                                  "workspace.content_browser.location.dependencies", "workspace.content_browser.location.tags"};
            for (std::size_t index = 0; index < icons.size(); ++index) {
                const ImVec2 position{minimum.x, minimum.y + 6.0F + static_cast<float>(index) * 32.0F};
                ImGui::SetCursorScreenPos(position);
                ImGui::PushID(static_cast<int>(index));
                static_cast<void>(ImGui::InvisibleButton("##location", {width - 1.0F, 30.0F}));
                const bool hovered = ImGui::IsItemHovered();
                const bool active = index == 0;
                if (active || hovered) {
                    ImVec4 fill = active ? Theme::Accent() : Theme::Text();
                    fill.w = active ? 0.055F : 0.025F;
                    drawList->AddRectFilled(position, {position.x + width - 1.0F, position.y + 30.0F}, Theme::U32(fill));
                }
                if (active)
                    drawList->AddRectFilled({position.x, position.y + 3.0F}, {position.x + 2.0F, position.y + 27.0F},
                                            Theme::U32(Theme::Accent()), 0.0F);
                constexpr float iconSize = 16.0F;
                Ui::DrawEditorIcon(drawList, icons[index], {position.x + (width - iconSize) * 0.5F, position.y + (30.0F - iconSize) * 0.5F},
                                   {iconSize, iconSize}, Theme::U32(active || hovered ? Theme::Text() : Theme::Dim()),
                                   context.theme.fonts.icon);
                if (hovered)
                    Ui::ShowTooltip(context.localization.Get("editor", keys[index]).c_str(), &context.theme.fonts);
                ImGui::PopID();
            }
        }

        [[nodiscard]] std::string EntrySecondaryText(const ContentBrowserEntry &entry, const ILocalizationService &localization) {
            if (entry.kind == ContentBrowserEntryKind::Directory) {
                const std::string &unit =
                    localization.Get("editor", entry.containedItemCount == 1 ? "workspace.content_browser.count.item"
                                                                             : "workspace.content_browser.count.items");
                return std::format("{} {}", entry.containedItemCount, unit);
            }
            return entry.assetType;
        }

        void DrawAssetFooter(const ImVec2 minimum, const float width, const std::size_t visibleCount,
                             const ContentBrowserDirectory &directory, const ILocalizationService &localization, ImFont *font) {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const ImVec2 maximum{minimum.x + width, minimum.y + AssetBrowserLayout::FooterHeight};
            drawList->AddRectFilled(minimum, maximum, Theme::U32(Theme::Mix(Theme::Bg0(), Theme::Bg1(), 0.55F)));
            drawList->AddLine(minimum, {maximum.x, minimum.y}, Theme::U32(Theme::Border()));
            const auto directoryCount =
                static_cast<std::size_t>(std::ranges::count_if(directory.entries, [](const ContentBrowserEntry &entry) {
                return entry.kind == ContentBrowserEntryKind::Directory;
            }));
            const bool onlyDirectories = visibleCount == directoryCount && visibleCount == directory.entries.size();
            const char *unitKey;
            if (onlyDirectories)
                unitKey = visibleCount == 1 ? "workspace.content_browser.count.folder" : "workspace.content_browser.count.folders";
            else
                unitKey = visibleCount == 1 ? "workspace.content_browser.count.item" : "workspace.content_browser.count.items";
            const std::string &unit = localization.Get("editor", unitKey);
            const std::string count = std::format("{} {}", visibleCount, unit);
            drawList->AddText(font, AssetBrowserLayout::SecondaryFontSize(), {minimum.x + 16.0F, minimum.y + 7.0F},
                              Theme::U32(Theme::Dim()), count.c_str());

            const char *statusKey = "workspace.content_browser.ready";
            if (directory.loadState == ContentBrowserLoadState::Loading)
                statusKey = "workspace.content_browser.loading";
            else if (directory.loadState == ContentBrowserLoadState::Error)
                statusKey = "workspace.content_browser.unavailable";
            const std::string &status = localization.Get("editor", statusKey);
            const auto statusSize = font->CalcTextSizeA(AssetBrowserLayout::SecondaryFontSize(), FLT_MAX, 0.0F, status.c_str());
            const float dotX = maximum.x - 16.0F;
            drawList->AddText(font, AssetBrowserLayout::SecondaryFontSize(), {dotX - 7.0F - statusSize.x, minimum.y + 7.0F},
                              Theme::U32(Theme::Dim()), status.c_str());
            if (directory.loadState == ContentBrowserLoadState::Ready) {
                drawList->AddCircleFilled({dotX, minimum.y + 14.0F}, 4.0F, Theme::U32(Theme::Ok()), 16);
            }
        }

        [[nodiscard]] const char *EmptyGridMessageKey(const ContentBrowserDirectory &directory) noexcept {
            if (directory.loadState == ContentBrowserLoadState::Loading)
                return "workspace.content_browser.loading";
            if (directory.loadState == ContentBrowserLoadState::Error)
                return "workspace.content_browser.unavailable";
            return directory.entries.empty() ? "workspace.content_browser.empty" : "workspace.content_browser.no_results";
        }

        void DrawEmptyAssetGrid(const ImVec2 gridOrigin, const float gridWidth, const float gridY, const ContentBrowserDirectory &directory,
                                const ILocalizationService &localization, ImFont *font, ImDrawList &drawList) {
            const ImVec4 color = directory.loadState == ContentBrowserLoadState::Error ? Theme::Err() : Theme::Dim();
            drawList.AddText(font, HeaderFontSize(), {gridOrigin.x, gridY}, Theme::U32(color),
                             localization.Get("editor", EmptyGridMessageKey(directory)).c_str());
            ImGui::SetCursorScreenPos({gridOrigin.x, gridY + PreviewRowHeight});
            ImGui::Dummy({gridWidth, 1.0F});
        }

        struct AssetEntryGridContext {
            ImVec2 origin;
            float width;
            float y;
            const std::vector<std::size_t> &visibleEntries;
            const ContentBrowserDirectory &directory;
            const EditorWorkspaceViewModel &viewModel;
            EditorWorkspaceViewCommandData &command;
            const EditorGuiContext &gui;
            AssetBrowserInteractionSession &interactionSession;
            AssetBrowserCardRenderer &cardRenderer;
            ImDrawList &drawList;
            ImFont *font;
        };

        struct AssetEntryGridMetrics {
            std::size_t columns;
            float cardWidth;
            float cardHeight;
            float previewWidth;
            float previewHeight;
            float gap;
        };

        [[nodiscard]] AssetEntryGridMetrics ResolveAssetEntryGridMetrics(const float width, const bool listView) noexcept {
            const AssetBrowserGridMetrics grid = ComputeAssetBrowserGridMetrics(width);
            if (listView) {
                return {.columns = 1U,
                        .cardWidth = std::min(720.0F, std::max(260.0F, width)),
                        .cardHeight = 48.0F,
                        .previewWidth = 54.0F,
                        .previewHeight = 47.0F,
                        .gap = 5.0F};
            }
            return {.columns = grid.columns,
                    .cardWidth = grid.cardWidth,
                    .cardHeight = AssetBrowserLayout::CardHeight,
                    .previewWidth = grid.cardWidth,
                    .previewHeight = AssetBrowserLayout::CardPreviewHeight,
                    .gap = AssetBrowserLayout::GridGap};
        }

        void DrawAssetEntry(const AssetEntryGridContext &context, const AssetEntryGridMetrics &metrics, const bool listView,
                            const std::size_t index) {
            const ContentBrowserEntry &entry = context.directory.entries[context.visibleEntries[index]];
            const std::size_t row = index / metrics.columns;
            const std::size_t column = index % metrics.columns;
            const ImVec2 interactionMin{context.origin.x + static_cast<float>(column) * (metrics.cardWidth + metrics.gap),
                                        context.y + static_cast<float>(row) * (metrics.cardHeight + metrics.gap)};
            ImGui::PushID(static_cast<int>(index));
            ImGui::SetCursorScreenPos(interactionMin);
            if (ImGui::InvisibleButton("##AssetCard", {metrics.cardWidth, metrics.cardHeight}))
                context.interactionSession.Select(entry.absolutePath);
            const bool hovered = ImGui::IsItemHovered();
            const bool cut = context.viewModel.contentBrowserClipboard.mode == ContentBrowserClipboardMode::Move &&
                             context.viewModel.contentBrowserClipboard.absoluteSourcePath == entry.absolutePath;
            const std::optional<std::string> draggedAssetPath = AbsoluteAssetPathFromPayload(ImGui::GetDragDropPayload());
            const bool dragging = draggedAssetPath.has_value() && *draggedAssetPath == entry.absolutePath;
            context.cardRenderer.Draw({.drawList = &context.drawList,
                                       .font = context.font,
                                       .iconFont = context.gui.theme.fonts.icon,
                                       .fontSize = CardFontSize(),
                                       .cardMin = {interactionMin.x, interactionMin.y - (hovered ? 1.0F : 0.0F)},
                                       .cardWidth = metrics.cardWidth,
                                       .cardHeight = metrics.cardHeight,
                                       .previewWidth = metrics.previewWidth,
                                       .previewHeight = metrics.previewHeight,
                                       .secondaryText = EntrySecondaryText(entry, context.gui.localization),
                                       .listView = listView,
                                       .hovered = hovered,
                                       .selected = context.interactionSession.State().selectedAbsolutePath == entry.absolutePath,
                                       .dimmed = cut || dragging},
                                      entry);
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.kind == ContentBrowserEntryKind::Directory) {
                    context.command = AssetBrowserInteractionSession::Navigate(entry.absolutePath);
                } else if (entry.kind == ContentBrowserEntryKind::Asset) {
                    context.command.command = EditorWorkspaceViewCommand::OpenSourceFile;
                    context.command.sourceOpenRequest = SourceOpenRequest{
                        .path = entry.absolutePath,
                        .origin = SourceOpenOrigin::AssetActivation,
                        .mode = SourceOpenMode::AllowExternalFallback,
                    };
                }
            }
            HandleAssetDragDropSource(entry, context.gui.localization);
            HandleDirectoryDragDropTarget(entry, interactionMin, metrics.cardWidth, metrics.cardHeight, &context.drawList, context.command);
            DrawAssetBrowserEntryActions(entry, context.viewModel, context.interactionSession, context.command, context.gui);
            ImGui::PopID();
        }

        void DrawAssetEntries(const AssetEntryGridContext &context) {
            const bool listView = context.interactionSession.State().viewMode == AssetBrowserViewMode::List;
            const AssetEntryGridMetrics metrics = ResolveAssetEntryGridMetrics(context.width, listView);
            for (std::size_t index = 0; index < context.visibleEntries.size(); ++index)
                DrawAssetEntry(context, metrics, listView, index);
            const std::size_t rowCount = (context.visibleEntries.size() + metrics.columns - 1U) / metrics.columns;
            const float gridHeight = static_cast<float>(rowCount) * metrics.cardHeight + static_cast<float>(rowCount - 1U) * metrics.gap;
            ImGui::SetCursorScreenPos({context.origin.x, context.y + gridHeight + AssetBrowserLayout::GridPaddingBottom});
            ImGui::Dummy({context.width, 1.0F});
            HandleAssetBrowserShortcuts(context.visibleEntries, context.viewModel, context.interactionSession, context.command);
        }
    }  // namespace

    AssetBrowserGridMetrics ComputeAssetBrowserGridMetrics(const float availableWidth) noexcept {
        const float safeWidth = std::max(1.0F, availableWidth);
        const float track = AssetBrowserLayout::CardMaximumWidth + AssetBrowserLayout::GridGap;
        const auto columns = static_cast<std::size_t>(std::max(1.0F, std::floor((safeWidth + AssetBrowserLayout::GridGap) / track)));
        const float cardWidth = std::min(safeWidth, AssetBrowserLayout::CardMaximumWidth);
        return {.columns = columns, .cardWidth = cardWidth};
    }

    /** @copydoc DrawAssetBrowserGrid */
    void DrawAssetBrowserGrid(const ImVec2 &contentOrigin, const float contentWidth, const EditorWorkspaceViewModel &viewModel,
                              EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                              AssetBrowserInteractionSession &interactionSession, AssetBrowserCardRenderer &cardRenderer) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImFont *font = ResolveFont(context.theme.fonts.sansCompact);
        AssetBrowserInteractionState &state = interactionSession.State();
        std::string &selectedAssetPath = state.selectedAbsolutePath;
        const ILocalizationService &localization = context.localization;
        const ContentBrowserDirectory &directory = viewModel.contentBrowser;
        const float contentHeight = std::max(1.0F, ImGui::GetContentRegionAvail().y);
        const float toolbarHeight = AssetBrowserLayout::ToolbarHeight;
        const float footerHeight = AssetBrowserLayout::FooterHeight;
        const float bodyHeight = std::max(1.0F, contentHeight - toolbarHeight - footerHeight);
        const ImVec2 contentMaximum{contentOrigin.x + contentWidth, contentOrigin.y + contentHeight};
        const ImVec2 toolbarMaximum{contentMaximum.x, contentOrigin.y + toolbarHeight};
        const ImVec2 bodyOrigin{contentOrigin.x, toolbarMaximum.y};
        const ImVec2 footerOrigin{contentOrigin.x, contentMaximum.y - footerHeight};

        drawList->AddRectFilled(contentOrigin, contentMaximum, Theme::U32(Theme::BottomDockContentSurface()));
        DrawGlobalDockToolbarSurface(contentOrigin, contentWidth, toolbarHeight);
        const ImVec2 toolbarPosition{contentOrigin.x + AssetBrowserLayout::ToolbarPaddingX,
                                     contentOrigin.y + (toolbarHeight - AssetBrowserLayout::ToolbarControlHeight) * 0.5F};
        DrawAssetBrowserToolbar(toolbarPosition, std::max(1.0F, contentWidth - AssetBrowserLayout::ToolbarPaddingX * 2.0F), viewModel,
                                command, state, context);

        const std::vector<std::size_t> visibleEntries = interactionSession.ProjectEntries(directory);
        if (!selectedAssetPath.empty() &&
            std::ranges::none_of(visibleEntries, [&directory, &selectedAssetPath](const std::size_t entryIndex) {
            return directory.entries[entryIndex].absolutePath == selectedAssetPath;
        })) {
            selectedAssetPath.clear();
        }
        cardRenderer.RetainVisible(directory, visibleEntries);

        DrawAssetLocationRail(bodyOrigin, bodyHeight, context);
        const ImVec2 gridViewportOrigin{bodyOrigin.x + AssetBrowserLayout::LocationRailWidth, bodyOrigin.y};
        const float gridViewportWidth = std::max(1.0F, contentWidth - AssetBrowserLayout::LocationRailWidth);
        ImGui::SetCursorScreenPos(gridViewportOrigin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0F);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::BottomDockContentSurface());
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, Theme::BorderStrong());
        ImGui::BeginChild("##AssetGridViewport", {gridViewportWidth, bodyHeight}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList *gridDrawList = ImGui::GetWindowDrawList();
        const ImVec2 gridOrigin{gridViewportOrigin.x + AssetBrowserLayout::GridPaddingX,
                                gridViewportOrigin.y + AssetBrowserLayout::GridPaddingTop};
        const float gridWidth = std::max(1.0F, gridViewportWidth - AssetBrowserLayout::GridPaddingX * 2.0F - 5.0F);
        float gridY = gridOrigin.y;
        if (!viewModel.contentBrowserOperationError.empty()) {
            const std::string &errorText = localization.Get("editor", viewModel.contentBrowserOperationError);
            gridDrawList->AddText(font, HeaderFontSize(), {gridOrigin.x, gridY}, Theme::U32(Theme::Err()), errorText.c_str());
            gridY += PreviewRowHeight + 4.0F;
        }

        if (const bool drawEntries = directory.loadState == ContentBrowserLoadState::Ready && !visibleEntries.empty(); !drawEntries) {
            DrawEmptyAssetGrid(gridOrigin, gridWidth, gridY, directory, localization, font, *gridDrawList);
        } else {
            DrawAssetEntries({.origin = gridOrigin,
                              .width = gridWidth,
                              .y = gridY,
                              .visibleEntries = visibleEntries,
                              .directory = directory,
                              .viewModel = viewModel,
                              .command = command,
                              .gui = context,
                              .interactionSession = interactionSession,
                              .cardRenderer = cardRenderer,
                              .drawList = *gridDrawList,
                              .font = font});
        }
        DrawAssetBrowserBackgroundActions(viewModel, interactionSession, command, context);
        ImGui::EndChild();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        DrawAssetFooter(footerOrigin, contentWidth, visibleEntries.size(), directory, localization, font);
        DrawAssetBrowserDialogs(state, directory, command, context);
    }
}  // namespace Horo::Editor
