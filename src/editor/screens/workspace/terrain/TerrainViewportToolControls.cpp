#include "Horo/Application/TerrainAuthoringCook.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "TerrainViewportTool.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        const std::string &Text(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key);
        }

        /** @brief Format a stable layer identity only when its captured semantic binding/locale changes. */
        std::string LayerLabel(const EditorGuiContext &context, Terrain::TerrainLayerId id, std::size_t index) {
            constexpr char Hex[] = "0123456789abcdef";
            std::string label = Text(context, "terrain.tools.layer") + " " + std::to_string(index + 1) + " · ";
            for (auto byte : id.Bytes()) {
                label += Hex[byte >> 4];
                label += Hex[byte & 15];
            }
            return label;
        }

        bool ActionEnabled(TerrainToolAction action, const TerrainDocumentView &view, bool eligible) {
            using enum Horo::Editor::TerrainToolAction;
            if (!eligible)
                return false;
            if (action == Undo)
                return view.canUndo;
            if (action == Redo)
                return view.canRedo;
            if (action == Save)
                return view.dirty;
            return true;
        }
    }  // namespace

    /** @copydoc TerrainViewportTool::ModeControls */
    void TerrainViewportTool::ModeControls(const Frame &, const EditorGuiContext &context) {
        const std::array choices{Text(context, "terrain.tools.sculpt").c_str(),        Text(context, "terrain.tools.weights").c_str(),
                                 Text(context, "terrain.tools.carve").c_str(),         Text(context, "terrain.tools.restore").c_str(),
                                 Text(context, "terrain.tools.foliage_paint").c_str(), Text(context, "terrain.tools.foliage_erase").c_str(),
                                 Text(context, "terrain.tools.foliage_select").c_str()};
        if (int mode = static_cast<int>(mode_);
            Ui::DrawComboPropRow(Text(context, "terrain.tools.tool").c_str(), "##terrain_tool", mode, choices, context.theme.fonts))
            mode_ = static_cast<TerrainToolMode>(mode);
        float radius = static_cast<float>(brush_.radiusMeters);
        if (Ui::DrawFloatPropRow(Text(context, "terrain.tools.radius").c_str(), "##terrain_radius", radius, context.theme.fonts).changed)
            brush_.radiusMeters = radius;
    }

    /** @copydoc TerrainViewportTool::SculptControls */
    void TerrainViewportTool::SculptControls(const EditorGuiContext &context) {
        int mode = static_cast<int>(brush_.mode);
        if (const std::array choices{Text(context, "terrain.tools.raise").c_str(), Text(context, "terrain.tools.lower").c_str(),
                                     Text(context, "terrain.tools.smooth").c_str(), Text(context, "terrain.tools.flatten").c_str(),
                                     Text(context, "terrain.tools.noise").c_str()};
            Ui::DrawComboPropRow(Text(context, "terrain.tools.operation").c_str(), "##terrain_sculpt_mode", mode, choices,
                                 context.theme.fonts))
            brush_.mode = static_cast<TerrainSculptMode>(mode);
        static_cast<void>(Ui::DrawFloatPropRow(Text(context, "terrain.tools.amount").c_str(), "##terrain_amount", brush_.amountMeters,
                                               context.theme.fonts));
        static_cast<void>(
            Ui::DrawFloatPropRow(Text(context, "terrain.tools.blend").c_str(), "##terrain_blend", brush_.blend, context.theme.fonts));
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.flatten_help"), Theme::Muted(), context.theme.fonts);
    }

    /** @copydoc TerrainViewportTool::LayerRow */
    void TerrainViewportTool::LayerRow(Terrain::TerrainLayerId layer, std::size_t index, const EditorGuiContext &context) {
        ImGui::PushID(static_cast<int>(index));
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, layerLabels_[index], Theme::Text(), context.theme.fonts);
        auto end = paint_.targets.begin() + paint_.targetCount;
        auto found = std::find_if(paint_.targets.begin(), end, [layer](const auto &target) {
            return target.layer == layer;
        });
        bool selected = found != end;
        ImGui::BeginDisabled(!selected && paint_.targetCount == paint_.targets.size());
        if (Ui::CheckboxControl(Text(context, "terrain.tools.active_layer").c_str(), &selected, context.theme.fonts)) {
            if (selected)
                paint_.targets[paint_.targetCount++] = {layer, 1};
            else {
                std::move(found + 1, end, found);
                paint_.targets[--paint_.targetCount] = {};
            }
        }
        ImGui::EndDisabled();
        end = paint_.targets.begin() + paint_.targetCount;
        found = std::find_if(paint_.targets.begin(), end, [layer](const auto &target) {
            return target.layer == layer;
        });
        if (found != end)
            static_cast<void>(Ui::DrawFloatPropRow(Text(context, "terrain.tools.target_weight").c_str(), "##layer_weight", found->weight,
                                                   context.theme.fonts));
        ImGui::PopID();
        ImGui::Spacing();
    }

    /** @copydoc TerrainViewportTool::LayerControls */
    void TerrainViewportTool::LayerControls(const Terrain::TerrainAuthoringLayerBinding &binding, std::size_t count,
                                            const EditorGuiContext &context) {
        if (const auto &locale = Text(context, "terrain.tools.layer");
            layerLabelsDigest_ != binding.digest || layerLabelsLocale_ != locale) {
            layerLabelsDigest_ = binding.digest;
            layerLabelsLocale_ = locale;
            for (std::size_t i = 0; i < count; ++i)
                layerLabels_[i] = LayerLabel(context, binding.order[i], i);
            // Removed meanings never silently retarget another stored channel.
            std::array<TerrainPaintTarget, 4> retained{};
            std::size_t used{};
            for (std::size_t i = 0; i < paint_.targetCount; ++i)
                if (std::find(binding.order.begin(), binding.order.begin() + count, paint_.targets[i].layer) !=
                    binding.order.begin() + count)
                    retained[used++] = paint_.targets[i];
            paint_.targets = retained;
            paint_.targetCount = used;
        }
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.four_layers"), Theme::Muted(), context.theme.fonts);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.weight_feedback"), Theme::Muted(),
                        context.theme.fonts);
        for (std::size_t i = 0; i < count; ++i)
            LayerRow(binding.order[i], i, context);
    }

    /** @copydoc TerrainViewportTool::PaintControls */
    void TerrainViewportTool::PaintControls(const Frame &frame, const EditorGuiContext &context) {
        if (mode_ != TerrainToolMode::Weights) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.holes_hint"), Theme::Muted(),
                            context.theme.fonts);
            return;
        }
        static_cast<void>(Ui::DrawFloatPropRow(Text(context, "terrain.tools.strength").c_str(), "##paint_strength", paint_.strength,
                                               context.theme.fonts));
        const auto *source = frame.source.BorrowSource();
        const auto *binding = frame.source.BorrowLayerBinding();
        if (!source || !binding)
            return;
        LayerControls(*binding, source->layerCount, context);
    }

    /** @copydoc TerrainViewportTool::FoliageControls */
    void TerrainViewportTool::FoliageControls(const Frame &frame, const EditorGuiContext &context) {
        const auto *active = frame.selection ? frame.selection->palette.Active() : nullptr;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, active ? active->label : Text(context, "terrain.tools.choose_foliage"),
                        Theme::Text(), context.theme.fonts);
        if (mode_ != TerrainToolMode::FoliagePaint) {
            static_cast<void>(
                Ui::CheckboxControl(Text(context, "terrain.tools.all_types").c_str(), &foliage_.allTypes, context.theme.fonts));
            return;
        }
        if (std::int32_t density = foliage_.densityPermille;
            Ui::DrawIntegerPropRow(Text(context, "terrain.tools.density").c_str(), "##foliage_density", density, context.theme.fonts)
                .changed)
            foliage_.densityPermille = static_cast<std::uint16_t>(std::clamp(density, 1, 1'000));
        static_cast<void>(
            Ui::DrawFloatPropRow(Text(context, "terrain.tools.scale").c_str(), "##foliage_scale", foliage_.scale, context.theme.fonts));
        static_cast<void>(Ui::DrawFloatPropRow(Text(context, "terrain.tools.offset").c_str(), "##foliage_offset", foliage_.offsetMeters,
                                               context.theme.fonts));
        static_cast<void>(
            Ui::DrawFloatPropRow(Text(context, "terrain.tools.yaw").c_str(), "##foliage_yaw", foliage_.yawRadians, context.theme.fonts));
    }

    /** @copydoc TerrainViewportTool::SourceControls */
    void TerrainViewportTool::SourceControls(const Frame &frame, std::optional<TerrainToolCommand> &command,
                                             const EditorGuiContext &context) {
        const auto view = frame.source.View();
        const auto layers = frame.source.BorrowLayerSets();
        if (layers.size() > 256) {
            Ui::ErrorText(Text(context, "terrain.tools.resources_overflow").c_str(), context.theme.fonts);
            return;
        }
        std::array<const char *, 257> labels{};
        labels[0] = Text(context, "terrain.tools.no_layers").c_str();
        for (std::size_t i = 0; i < layers.size(); ++i)
            labels[i + 1] = layers[i].label.c_str();
        if (conversionLayer_ > static_cast<int>(layers.size()))
            conversionLayer_ = 0;
        static_cast<void>(Ui::DrawComboPropRow(Text(context, "terrain.tools.layer_set").c_str(), "##terrain_layer_set", conversionLayer_,
                                               std::span(labels.data(), layers.size() + 1), context.theme.fonts));
        if (const bool convertible = !view.containerSource || (!view.hasLayerBinding && conversionLayer_ > 0);
            Ui::Button({.label = Text(context, "terrain.tools.enable_painting").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = frame.eligible && convertible,
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            command = Intent(frame, TerrainToolAction::ConvertSource);
            if (conversionLayer_ > 0)
                command->conversionLayers = layers[static_cast<std::size_t>(conversionLayer_ - 1)].binding;
            Cancel();
            ImGui::CloseCurrentPopup();
        }
        if (Ui::Button({.label = Text(context, "terrain.tools.refresh_resources").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = frame.eligible}))
            command = Intent(frame, TerrainToolAction::RefreshFoliage);
    }

    /** @copydoc TerrainViewportTool::ActionControls */
    void TerrainViewportTool::ActionControls(const Frame &frame, std::optional<TerrainToolCommand> &command,
                                             const EditorGuiContext &context) {
        using enum Horo::Editor::TerrainToolAction;
        const auto view = frame.source.View();
        const std::array actions{std::pair{Undo, "terrain.tools.undo"}, std::pair{Redo, "terrain.tools.redo"},
                                 std::pair{Save, "terrain.tools.save"}, std::pair{Close, "terrain.tools.close"},
                                 std::pair{Discard, "terrain.tools.discard"}};
        for (const auto &[action, key] : actions)
            if (Ui::Button({.label = Text(context, key).c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = ActionEnabled(action, view, frame.eligible),
                            .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                command = Intent(frame, action);
                Cancel();
                ImGui::CloseCurrentPopup();
            }
    }

    /** @brief Render the retained actual cook receipt even when its source is no longer eligible for adoption. */
    void TerrainViewportTool::CookStatus(const Frame &frame, const EditorGuiContext &context) {
        const char *key = frame.cookRunning   ? "terrain.tools.cooking"
                          : frame.cookCurrent ? "terrain.tools.cooked"
                                              : "terrain.tools.cook_hint";
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, key), Theme::Muted(), context.theme.fonts);
        if (!frame.cookOutcome)
            return;
        if (frame.cookOutcome->result.HasError()) {
            const auto &error = frame.cookOutcome->result.ErrorValue();
            Ui::ErrorText(error.code.Value().c_str(), context.theme.fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, error.message, Theme::Err(), context.theme.fonts);
        } else if (!frame.cookCurrent) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.cook_stale"), Theme::Muted(),
                            context.theme.fonts);
        }
    }

    /** @copydoc TerrainViewportTool::CookControls */
    void TerrainViewportTool::CookControls(const Frame &frame, std::optional<TerrainToolCommand> &command,
                                           const EditorGuiContext &context) {
        if (const auto view = frame.source.View();
            Ui::Button({.label = Text(context, "terrain.tools.cook").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = frame.eligible && frame.cookAvailable && !frame.cookRunning && !view.dirty && view.containerSource})) {
            command = Intent(frame, TerrainToolAction::Cook);
            Cancel();
            ImGui::CloseCurrentPopup();
        }
        if (Ui::Button({.label = Text(context, "terrain.tools.cancel_cook").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = frame.eligible && frame.cookRunning})) {
            command = Intent(frame, TerrainToolAction::CancelCook);
            Cancel();
            ImGui::CloseCurrentPopup();
        }
        CookStatus(frame, context);
    }

    /** @copydoc TerrainViewportTool::Controls */
    bool TerrainViewportTool::Controls(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context) {
        const auto cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos({frame.origin.x + Ui::ScaledLayoutValue(8), frame.origin.y + Ui::ScaledLayoutValue(44)});
        if (Ui::Button({.label = Text(context, "terrain.tools.title").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = frame.eligible,
                        .componentSize = Ui::ComponentSize::Small})) {
            Cancel();
            ImGui::OpenPopup("##terrain_tools");
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetNextWindowSizeConstraints({std::max(1.F, std::min(frame.size.x - 16.F, Ui::ScaledLayoutValue(320))), 0},
                                            {std::max(1.F, frame.size.x - 16.F), std::max(1.F, frame.size.y - 16.F)});
        const bool open = ImGui::BeginPopup("##terrain_tools");
        if (open) {
            ModeControls(frame, context);
            if (mode_ == TerrainToolMode::Sculpt)
                SculptControls(context);
            else if (mode_ <= TerrainToolMode::RestoreHole)
                PaintControls(frame, context);
            else
                FoliageControls(frame, context);
            if (!ModeAvailable(frame))
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.mode_unavailable"), Theme::Muted(),
                                context.theme.fonts);
            SourceControls(frame, command, context);
            ActionControls(frame, command, context);
            CookControls(frame, command, context);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "terrain.tools.instructions"), Theme::Muted(),
                            context.theme.fonts);
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(cursor);
        return open || hovered;
    }
}  // namespace Horo::Editor
