#include "editor/screens/workspace/panels/world_composition/WorldCompositionTab.h"

#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/WorldLayerOutliner.h"

#include <array>

namespace Horo::Editor {
    /** @copydoc WorldCompositionTab::OnDetach */
    void WorldCompositionTab::OnDetach() {
        source_.reset();
        path_.clear();
        layers_.clear();
        cells_.clear();
        localeLabel_.clear();
        layerLabels_.clear();
    }

    /** @copydoc WorldCompositionTab::DrawIcon */
    void WorldCompositionTab::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const auto inset = size.x * 0.2F;
        drawList->AddRect({position.x + inset, position.y + inset}, {position.x + size.x - inset, position.y + size.y - inset}, color);
        drawList->AddLine({position.x + size.x * 0.5F, position.y + inset}, {position.x + size.x * 0.5F, position.y + size.y - inset},
                          color);
        drawList->AddLine({position.x + inset, position.y + size.y * 0.5F}, {position.x + size.x - inset, position.y + size.y * 0.5F},
                          color);
    }

    /** @copydoc WorldCompositionTab::RefreshRows */
    void WorldCompositionTab::RefreshRows(const EditorWorkspaceViewModel &vm, const EditorGuiContext &context) {
        const auto text = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        if (source_ == vm.worldDocument && query_ == vm.worldBrowserQuery && localeLabel_ == text("workspace.world.closed"))
            return;
        source_ = vm.worldDocument;
        query_ = vm.worldBrowserQuery;
        localeLabel_ = text("workspace.world.closed");
        layers_.clear();
        cells_.clear();
        layerLabels_.clear();
        layerLabels_.push_back(text("workspace.world.all_layers").c_str());
        if (source_)
            for (const auto &layer : source_->document.layers)
                layerLabels_.push_back(layer.name.c_str());
        layerColumns_ = {{{"name", text("workspace.world.layer")},
                          {"id", text("workspace.world.id")},
                          {"status", text("workspace.world.status")},
                          {"objects", text("workspace.world.objects")},
                          {"always", text("workspace.world.always_present")}}};
        cellColumns_ = {{{"coordinate", text("workspace.world.coordinate")},
                         {"layer", text("workspace.world.layer")},
                         {"lod", text("workspace.world.lod")},
                         {"status", text("workspace.world.status")},
                         {"objects", text("workspace.world.objects")}}};
        textColor_ = {};
        if (!vm.worldBrowser || vm.worldBrowser->source != vm.worldDocument)
            return;
        const auto status = [&](const std::uint32_t total, const std::uint32_t open) -> const std::string & {
            if (open == 0)
                return text("workspace.world.closed");
            return text(open == total ? "workspace.world.open" : "workspace.world.mixed");
        };
        for (const auto &layer : vm.worldBrowser->layers)
            layers_.push_back({.cells = {{layer.descriptor.name},
                                         {std::to_string(layer.descriptor.id.Value())},
                                         {status(layer.objects, layer.openObjects)},
                                         {std::to_string(layer.objects)},
                                         {std::to_string(layer.alwaysPresentObjects)}}});
        for (const auto &cell : vm.worldBrowser->cells) {
            const auto &id = cell.id;
            cells_.push_back({.cells = {{std::to_string(id.x) + ", " + std::to_string(id.y) + ", " + std::to_string(id.z)},
                                        {std::to_string(id.layer.Value())},
                                        {std::to_string(id.lod)},
                                        {status(cell.objects, cell.openObjects)},
                                        {std::to_string(cell.objects)}}});
        }
    }

    /** @copydoc WorldCompositionTab::DrawAdmission */
    bool WorldCompositionTab::DrawAdmission(const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &command,
                                            const EditorGuiContext &context) {
        const auto text = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const auto &fonts = context.theme.fonts;
        Ui::SectionTitle(text("workspace.panel.world_composition").c_str(), fonts);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.world.source_hint"), Theme::Muted(), fonts);
        static_cast<void>(Ui::InputTextControl("##world_source", path_, 4096, fonts,
                                               {.hint = text("workspace.world.path").c_str(), .componentSize = Ui::ComponentSize::Medium}));
        if (Ui::Button({.label = text("workspace.world.open_source").c_str(), .enabled = !path_.empty(), .font = fonts.sans})) {
            command.command = EditorWorkspaceViewCommand::OpenWorldCompositionSource;
            command.stringPayload = path_;
            command.worldDocumentFence = vm.worldDocument ? std::optional{vm.worldDocument->fence} : std::nullopt;
        }
        if (vm.worldDocumentError) {
            Ui::ErrorText(text("workspace.world.error").c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, vm.worldDocumentError->code.Value(), Theme::Text(), fonts);
        }
        if (!vm.worldDocument) {
            source_.reset();
            layers_.clear();
            cells_.clear();
            layerLabels_.clear();
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.world.no_document"), Theme::Muted(), fonts);
            return false;
        }
        return true;
    }

    /** @copydoc WorldCompositionTab::DrawFilters */
    void WorldCompositionTab::DrawFilters(const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &command,
                                          const EditorGuiContext &context) {
        const auto text = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const auto &fonts = context.theme.fonts;
        ImGui::BeginDisabled(command.command != EditorWorkspaceViewCommand::None);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, vm.worldDocumentSource, Theme::Text(), fonts);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.world.authoring_status"), Theme::Muted(), fonts);
        if (Ui::Button({.label = text("workspace.world.close_source").c_str(), .font = fonts.sans})) {
            command.command = EditorWorkspaceViewCommand::CloseWorldComposition;
            command.worldDocumentFence = vm.worldDocument->fence;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(command.command != EditorWorkspaceViewCommand::None);
        RefreshRows(vm, context);
        draft_ = vm.worldBrowserQuery;
        auto &query = draft_;
        static_cast<void>(
            Ui::InputTextControl("##world_search", query.search, 257, fonts,
                                 {.hint = text("workspace.world.search").c_str(), .componentSize = Ui::ComponentSize::Medium}));
        const std::array<const char *, 4> labels{text("workspace.world.all").c_str(), text("workspace.world.open").c_str(),
                                                 text("workspace.world.closed").c_str(), text("workspace.world.mixed").c_str()};
        int status = static_cast<int>(query.status);
        if (Ui::ComboControl("##world_status", &status, labels.data(), static_cast<int>(labels.size()), fonts))
            query.status = static_cast<WorldBrowserStatus>(status);
        int layerIndex{};
        for (std::size_t index = 0; index < vm.worldDocument->document.layers.size(); ++index)
            if (query.layer == vm.worldDocument->document.layers[index].id)
                layerIndex = static_cast<int>(index + 1);
        if (Ui::ComboControl("##world_layer", &layerIndex, layerLabels_.data(), static_cast<int>(layerLabels_.size()), fonts))
            query.layer = layerIndex == 0 ? std::nullopt
                                          : std::optional{vm.worldDocument->document.layers[static_cast<std::size_t>(layerIndex - 1)].id};
        if (query != vm.worldBrowserQuery) {
            command.command = EditorWorkspaceViewCommand::QueryWorldComposition;
            command.worldBrowserQuery = query;
            command.worldDocumentFence = vm.worldDocument->fence;
        }
        ImGui::EndDisabled();
    }

    /** @copydoc WorldCompositionTab::DrawTables */
    void WorldCompositionTab::DrawTables(const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &command,
                                         const EditorGuiContext &context) {
        const auto text = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const auto &fonts = context.theme.fonts;
        const auto color = Theme::Text();
        if (textColor_.x != color.x || textColor_.y != color.y || textColor_.z != color.z || textColor_.w != color.w) {
            textColor_ = color;
            for (auto *rows : {&layers_, &cells_})
                for (auto &row : *rows)
                    for (auto &cell : row.cells)
                        cell.color = color;
        }
        ImGui::BeginDisabled(command.command != EditorWorkspaceViewCommand::None);
        RouteWorldLayerOutlinerIntent(DrawWorldLayerOutliner(*vm.worldDocument, context), command);
        ImGui::EndDisabled();
        if (vm.worldLayerFailure)
            Ui::ErrorText(text("workspace.world.error").c_str(), fonts);
        Ui::SectionTitle(text("workspace.world.layers").c_str(), fonts);
        static_cast<void>(Ui::DrawTable({.id = "##world_layers", .componentSize = Ui::ComponentSize::Medium, .selectableCells = false},
                                        layerColumns_, layers_, fonts));
        Ui::SectionTitle(text("workspace.world.cells").c_str(), fonts);
        static_cast<void>(Ui::DrawTable({.id = "##world_cells", .componentSize = Ui::ComponentSize::Medium, .selectableCells = false},
                                        cellColumns_, cells_, fonts));
        if (!vm.worldDocumentError && layers_.empty() && cells_.empty())
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.world.no_matches"), Theme::Muted(), fonts);
    }

    /** @copydoc WorldCompositionTab::DrawPanel */
    void WorldCompositionTab::DrawPanel(const ImVec2 &, const ImVec2 &, const EditorWorkspaceViewModel &vm,
                                        EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (!DrawAdmission(vm, command, context))
            return;
        DrawFilters(vm, command, context);
        DrawTables(vm, command, context);
    }
}  // namespace Horo::Editor
