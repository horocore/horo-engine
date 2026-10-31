#include "FoliagePaletteTab.h"
#include "TerrainDocumentHost.h"
#include "TerrainToolSelection.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

namespace Horo::Editor {
    namespace {
        /** @brief Emit one exact typed palette action; all mutable source work stays outside Draw. */
        void Emit(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command, TerrainToolAction action,
                  std::optional<Terrain::FoliageAuthoringDefinitionReference> type = {}) {
            auto intent = std::make_shared<TerrainToolCommand>();
            intent->action = action; intent->document = model.terrainPaletteDocument->View(); intent->foliageType = std::move(type);
            command.command = EditorWorkspaceViewCommand::TerrainTool; command.terrainTool = std::move(intent);
        }
    }
    /** @copydoc FoliagePaletteTab::DrawIcon */
    void FoliagePaletteTab::DrawIcon(ImDrawList *draw, const ImVec2 &position, const ImVec2 &size, ImU32 color) {
        const ImVec2 tip{position.x + size.x * .5F, position.y + size.y * .15F};
        draw->AddTriangle(tip, {position.x + size.x * .2F, position.y + size.y * .7F},
                          {position.x + size.x * .8F, position.y + size.y * .7F}, color);
        draw->AddLine({tip.x, position.y + size.y * .55F}, {tip.x, position.y + size.y * .9F}, color);
    }
    /** @copydoc FoliagePaletteTab::DrawTypes */
    void FoliagePaletteTab::DrawTypes(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                                      const EditorGuiContext &context) {
        const auto &state = *model.terrainToolSelection;
        const auto *selected = state.palette.Active();
        const auto items = state.palette.Items();
        if (items.empty()) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context.localization, "foliage.empty"),
                            Theme::Muted(), context.theme.fonts);
            return;
        }
        for (std::size_t index = 0; index < items.size(); ++index) {
            const auto &item = items[index];
            const bool active = selected && selected->source.type == item.source.type;
            ImGui::PushID(static_cast<int>(index));
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, item.label, Theme::Text(), context.theme.fonts);
            if (Ui::Button({.label = kTextScope.Text(context.localization, active ? "foliage.selected" : "foliage.select_type").c_str(),
                .variant = active ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                .enabled = model.playState == EditorPlayState::Idle, .style = {.width = Ui::StyleWidth::FillAvailable}}))
                Emit(model, command, TerrainToolAction::ChooseFoliage, item.source);
            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    /** @copydoc FoliagePaletteTab::DrawCreation */
    void FoliagePaletteTab::DrawCreation(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                                         const EditorGuiContext &context) {
        const bool enabled=model.terrainAssetCreationAvailable && model.playState==EditorPlayState::Idle;
        if (Ui::Button({.label=kTextScope.Text(context.localization,"foliage.create_type").c_str(),.enabled=enabled})) {
            command.command=EditorWorkspaceViewCommand::CreateTerrainFoliageType;
            command.documentInstance=model.terrainPaletteDocument->Identity().instance;
        }
        if (Ui::Button({.label=kTextScope.Text(context.localization,"foliage.create_layers").c_str(),.variant=Ui::ButtonVariant::Secondary,.enabled=enabled})) {
            command.command=EditorWorkspaceViewCommand::CreateTerrainLayerSet;
            command.documentInstance=model.terrainPaletteDocument->Identity().instance;
        }
    }
    /** @copydoc FoliagePaletteTab::DrawPanel */
    void FoliagePaletteTab::DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &model,
                                      EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        ImGui::SetCursorScreenPos(position);
        ImGui::BeginChild("##foliage_palette", size);
        Ui::SectionTitle(kTextScope.Text(context.localization, "panel.foliage_palette").c_str(), context.theme.fonts);
        if (!model.terrainPaletteDocument || !model.terrainToolSelection || model.terrainPaletteDocument->View().closed ||
            model.terrainToolSelection->document != model.terrainPaletteDocument->Identity().instance) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context.localization, "foliage.choose_terrain"),
                            Theme::Muted(), context.theme.fonts);
            ImGui::EndChild(); return;
        }
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, kTextScope.Text(context.localization, "foliage.authoring_hint"),
                        Theme::Muted(), context.theme.fonts);
        if (Ui::Button({.label = kTextScope.Text(context.localization, "foliage.refresh").c_str(),
            .variant = Ui::ButtonVariant::Secondary, .enabled = model.playState == EditorPlayState::Idle}))
            Emit(model, command, TerrainToolAction::RefreshFoliage);
        DrawCreation(model,command,context);
        DrawTypes(model, command, context);
        ImGui::EndChild();
    }
}
