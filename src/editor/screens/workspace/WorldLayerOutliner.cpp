#include "editor/screens/workspace/WorldLayerOutliner.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Draws one localized shared action without allowing a second action to overwrite the first. */
        bool Action(const char *key, const bool enabled, const EditorGuiContext &context) {
            const auto label = context.localization.Get("editor", key);
            return Ui::Button(
                {.label = label.c_str(), .variant = Ui::ButtonVariant::Secondary, .enabled = enabled, .font = context.theme.fonts.sans});
        }

        /** @brief Emits a canonical command from one stable row; ordinals refer to the full source order. */
        void DrawLayerRow(const PartitionedWorldSnapshot &snapshot, const std::size_t ordinal, const EditorGuiContext &context,
                          WorldLayerOutlinerIntent &intent) {
            using enum WorldLayerOutlinerAction;
            const auto &layer = snapshot.document.layers[ordinal];
            const bool selected = snapshot.currentLayer == layer.id;
            ImGui::PushID(static_cast<int>(layer.id.Value()));
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, layer.name, Theme::Text(), context.theme.fonts);
            const auto &font = context.theme.fonts;
            const auto current = context.localization.Get("editor", "workspace.world_layers.current");
            if (selected) {
                // Badge deliberately leaves an inline cursor; this row owns a separate status line.
                ImGui::BeginGroup();
                Ui::Badge({.label = current.c_str(), .tone = Ui::BadgeTone::Accent}, font);
                ImGui::EndGroup();
            }
            if (Action("workspace.world_layers.set_current", intent.action == None && !selected, context)) {
                intent.action = Execute;
                intent.command = SetCurrentWorldLayer{layer.id};
            }
            // Vertical controls remain usable at narrow widths and with long localized labels.
            if (Action("workspace.world_layers.move_up", intent.action == None && ordinal != 0, context)) {
                intent.action = Execute;
                intent.command = ReorderWorldLayer{layer.id, ordinal - 1};
            }
            if (Action("workspace.world_layers.move_down", intent.action == None && ordinal + 1 < snapshot.document.layers.size(),
                       context)) {
                intent.action = Execute;
                intent.command = ReorderWorldLayer{layer.id, ordinal + 1};
            }
            Ui::DashedSeparator();
            ImGui::PopID();
        }

        /** @brief Emits only one filter intent from shared text and ownership controls. */
        void DrawLayerFilters(const PartitionedWorldSnapshot &snapshot, const EditorGuiContext &context, WorldLayerOutlinerIntent &intent) {
            using enum WorldLayerOutlinerAction;
            const auto &fonts = context.theme.fonts;
            std::string filter = snapshot.layerFilter.name;
            const auto hint = context.localization.Get("editor", "workspace.world_layers.filter");
            ImGui::BeginDisabled(intent.action != None);
            if (Ui::InputTextControl("##WorldLayerFilter", filter, 1025, fonts, {.hint = hint.c_str()})) {
                intent.action = Execute;
                intent.command = FilterWorldLayers{{std::move(filter), snapshot.layerFilter.ownership}};
            }
            ImGui::EndDisabled();
            const std::array<std::string, 4> ownershipLabels{context.localization.Get("editor", "workspace.world_layers.all_owners"),
                                                             context.localization.Get("editor", "workspace.world_layers.streaming"),
                                                             context.localization.Get("editor", "workspace.world_layers.gameplay"),
                                                             context.localization.Get("editor", "workspace.world_layers.network")};
            const std::array<const char *, 4> ownershipItems{ownershipLabels[0].c_str(), ownershipLabels[1].c_str(),
                                                             ownershipLabels[2].c_str(), ownershipLabels[3].c_str()};
            int ownership = snapshot.layerFilter.ownership ? static_cast<int>(*snapshot.layerFilter.ownership) + 1 : 0;
            ImGui::BeginDisabled(intent.action != None);
            if (Ui::ComboControl("##WorldLayerOwnership", &ownership, ownershipItems.data(), static_cast<int>(ownershipItems.size()),
                                 fonts)) {
                auto next = snapshot.layerFilter;
                next.ownership = ownership ? std::optional{static_cast<WS::WorldLayerOwnership>(ownership - 1)} : std::nullopt;
                intent.action = Execute;
                intent.command = FilterWorldLayers{std::move(next)};
            }
            ImGui::EndDisabled();
        }

        /** @brief Projects admitted source rows without retargeting filtered authoring context. */
        void DrawLayerRows(const PartitionedWorldSnapshot &snapshot, const EditorGuiContext &context, WorldLayerOutlinerIntent &intent) {
            const auto &fonts = context.theme.fonts;
            bool any{};
            bool currentVisible{};
            for (std::size_t ordinal = 0; ordinal < snapshot.document.layers.size(); ++ordinal) {
                const auto &layer = snapshot.document.layers[ordinal];
                if ((snapshot.layerFilter.ownership && layer.ownership != *snapshot.layerFilter.ownership) ||
                    layer.name.find(snapshot.layerFilter.name) == std::string::npos)
                    continue;
                any = true;
                currentVisible = currentVisible || snapshot.currentLayer == layer.id;
                DrawLayerRow(snapshot, ordinal, context, intent);
            }
            if (!any)
                Ui::Hint(context.localization.Get("editor", "workspace.world_layers.empty").c_str(), fonts);
            if (snapshot.currentLayer && !currentVisible)
                Ui::Hint(context.localization.Get("editor", "workspace.world_layers.current_hidden").c_str(), fonts);
        }
    }  // namespace

    /** @copydoc RouteWorldLayerOutlinerIntent */
    void RouteWorldLayerOutlinerIntent(WorldLayerOutlinerIntent intent, EditorWorkspaceViewCommandData &command) {
        using enum WorldLayerOutlinerAction;
        if (intent.action == None)
            return;
        command.command =
            intent.action == Create ? EditorWorkspaceViewCommand::OpenCreateWorldLayer : EditorWorkspaceViewCommand::WorldLayerAuthoring;
        command.worldLayerIntent = std::move(intent);
    }

    /** @copydoc DrawWorldLayerOutliner */
    WorldLayerOutlinerIntent DrawWorldLayerOutliner(const PartitionedWorldSnapshot &snapshot, const EditorGuiContext &context) {
        using enum WorldLayerOutlinerAction;
        WorldLayerOutlinerIntent intent{.expected = snapshot.fence};
        const auto &fonts = context.theme.fonts;
        const auto title = context.localization.Get("editor", "workspace.world_layers.title");
        Ui::SectionTitle(title.c_str(), fonts);
        if (Action("workspace.world_layers.create", true, context))
            intent.action = Create;
        if (Action("workspace.world_layers.undo", intent.action == None && snapshot.layerUndoCount != 0, context))
            intent.action = Undo;
        if (Action("workspace.world_layers.redo", intent.action == None && snapshot.layerRedoCount != 0, context))
            intent.action = Redo;

        DrawLayerFilters(snapshot, context, intent);
        if (Action("workspace.world_layers.clear_current", intent.action == None && snapshot.currentLayer.has_value(), context)) {
            intent.action = Execute;
            intent.command = SetCurrentWorldLayer{};
        }

        DrawLayerRows(snapshot, context, intent);
        return intent;
    }
}  // namespace Horo::Editor
