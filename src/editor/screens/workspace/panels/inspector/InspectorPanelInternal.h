#pragma once

/**
 * @file InspectorPanelInternal.h
 * @brief Private shared drawing helpers for Inspector panel translation units.
 */

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
#include <utility>
#include <variant>

namespace Horo::Editor::InspectorPanelDetail {
    constexpr float DegreesToRadians = std::numbers::pi_v<float> / 180.0F;

    /** @brief Maps each typed scene-object kind to the shared editor icon registry. */
    [[nodiscard]] inline Ui::UiIcon KindIcon(const SceneObjectKind kind) noexcept {
        using enum SceneObjectKind;
        switch (kind) {
            case Mesh:
                return Ui::UiIcon::HierarchyMesh;
            case Camera:
                return Ui::UiIcon::Camera;
            case Light:
                return Ui::UiIcon::Light;
            case TriggerVolume:
                return Ui::UiIcon::TriggerVolume;
            case AudioSource:
                return Ui::UiIcon::AudioSource;
            case GameObject:
                return Ui::UiIcon::HierarchyGeneric;
        }
        return Ui::UiIcon::HierarchyGeneric;
    }

    /** @brief Reports whether the Inspector's degree-based perspective draft is valid. */
    [[nodiscard]] inline bool IsValidFieldOfViewDegrees(const float value) noexcept {
        return std::isfinite(value) && value > 0.0F && value < 180.0F;
    }

    /** @brief Resolves the selected object without exposing iterator details to the panel flow. */
    [[nodiscard]] inline const SceneObject *FindSelectedObject(const EditorWorkspaceViewModel &viewModel) noexcept {
        if (!viewModel.primarySelection.has_value())
            return nullptr;
        const auto selected = std::ranges::find(viewModel.objects, *viewModel.primarySelection, &SceneObject::id);
        return selected == viewModel.objects.end() ? nullptr : std::to_address(selected);
    }

    /** @brief Inspector-owned meaning attached to the generic card-title action surface. */
    struct ComponentTitleBarResult {
        bool resetRequested{false};
        bool toggleEnabledRequested{false};
        bool removeRequested{false};
    };

    /** @brief Component capabilities projected onto generic card actions for one frame. */
    struct ComponentTitleBarOptions {
        bool enabled{true};
        bool canReset{true};
        bool canToggleEnabled{true};
        bool canRemove{true};
    };

    /** @brief Reports Escape only when the Inspector card child window owns focus. */
    [[nodiscard]] inline bool IsInspectorCardEscapeRequested() noexcept {
        return ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    }

    /** @brief Localized labels consumed while composing one component title bar. */
    struct ComponentTitleBarLabels {
        const char *reset;
        const char *enable;
        const char *disable;
        const char *settings;
        const char *remove;
    };

    /** @brief Resolves the localized component-action labels for the current frame. */
    [[nodiscard]] inline ComponentTitleBarLabels ResolveComponentTitleBarLabels(const EditorGuiContext &context) {
        return {
            .reset = context.localization.Get("editor", "workspace.inspector.component.reset").c_str(),
            .enable = context.localization.Get("editor", "workspace.inspector.component.enable").c_str(),
            .disable = context.localization.Get("editor", "workspace.inspector.component.disable").c_str(),
            .settings = context.localization.Get("editor", "workspace.inspector.component.settings").c_str(),
            .remove = context.localization.Get("editor", "workspace.inspector.remove_component").c_str(),
        };
    }

    /** @brief Populates the enabled component actions exposed through the settings menu. */
    [[nodiscard]] inline std::size_t PopulateComponentMenuItems(const ComponentTitleBarOptions options,
                                                                const ComponentTitleBarLabels &labels, ComponentTitleBarResult &result,
                                                                std::array<Ui::CardMenuAction, 3> &items) {
        std::size_t count = 0;
        if (options.canReset)
            items[count++] = {.id = "reset", .label = labels.reset, .icon = Ui::UiIcon::Reset, .onInvoke = [&result] {
                result.resetRequested = true;
            }};
        if (options.canToggleEnabled)
            items[count++] = {.id = "toggle_enabled",
                              .label = options.enabled ? labels.disable : labels.enable,
                              .icon = options.enabled ? Ui::UiIcon::Check : Ui::UiIcon::CheckboxUnchecked,
                              .onInvoke = [&result] {
                result.toggleEnabledRequested = true;
            }};
        if (options.canRemove)
            items[count++] = {.id = "remove",
                              .label = labels.remove,
                              .icon = Ui::UiIcon::Delete,
                              .destructive = true,
                              .separatorBefore = count > 0,
                              .onInvoke = [&result] {
                result.removeRequested = true;
            }};
        return count;
    }

    /** @brief Binds Inspector semantics and localized copy to a generic card title bar. */
    [[nodiscard]] inline ComponentTitleBarResult DrawComponentTitleBar(Ui::Card &card, const char *title,
                                                                       const ComponentTitleBarOptions options,
                                                                       const EditorGuiContext &context) {
        ComponentTitleBarResult result;
        const ComponentTitleBarLabels labels = ResolveComponentTitleBarLabels(context);
        std::array<Ui::CardMenuAction, 3> menuItems;
        const std::size_t menuItemCount = PopulateComponentMenuItems(options, labels, result, menuItems);
        const std::span<const Ui::CardMenuAction> menu{menuItems.data(), menuItemCount};
        const std::array actions{
            Ui::CardTitleBarAction{
                .id = "reset",
                .icon = Ui::UiIcon::Reset,
                .title = labels.reset,
                .enabled = options.canReset,
                .onInvoke =
                    [&result] {
            result.resetRequested = true;
        },
            },
            Ui::CardTitleBarAction{
                .id = "enabled",
                .icon = options.enabled ? Ui::UiIcon::Check : Ui::UiIcon::CheckboxUnchecked,
                .title = options.enabled ? labels.disable : labels.enable,
                .enabled = options.canToggleEnabled,
                .active = options.enabled,
                .onInvoke =
                    [&result] {
            result.toggleEnabledRequested = true;
        },
            },
            Ui::CardTitleBarAction{
                .id = "settings",
                .icon = Ui::UiIcon::Settings,
                .title = labels.settings,
                .enabled = !menu.empty(),
                .menuItems = menu,
            },
        };
        card.DrawTitleBar({.id = "title_bar", .title = title, .fonts = context.theme.fonts, .actions = actions});
        return result;
    }

    /** @brief Draws one localized Inspector validation message with consistent spacing. */
    inline void DrawValidationMessageIfInvalid(const bool valid, const std::string &message, const Theme::Fonts &fonts,
                                               const float topSpacing, const float bottomSpacing = 0.0F) {
        if (valid)
            return;
        ImGui::SetCursorPosX(14.0F);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + topSpacing);
        Ui::ErrorText(message.c_str(), fonts);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + bottomSpacing);
    }

    struct BehaviorFieldVisitor {
        Gameplay::BehaviorField &field;
        const std::span<const char *const, 2> enabledEntries;
        const EditorGuiContext &context;

        bool operator()(bool &fieldVal) const {
            if (int selected = fieldVal ? 1 : 0;
                Ui::DrawComboPropRow(field.name.c_str(), "value", selected, enabledEntries, context.theme.fonts)) {
                fieldVal = selected != 0;
                return true;
            }
            return false;
        }

        bool operator()(double &fieldVal) const {
            auto draft = static_cast<float>(fieldVal);
            if (const auto [changed, committed] = Ui::DrawFloatPropRow(field.name.c_str(), "value", draft, context.theme.fonts);
                committed) {
                fieldVal = static_cast<double>(draft);
                return true;
            }
            return false;
        }

        bool operator()(std::int64_t &fieldVal) const {
            auto draft = static_cast<float>(fieldVal);
            const auto [changed, committed] =
                Ui::DrawFloatPropRow(field.name.c_str(), "value", draft, context.theme.fonts, Ui::FloatPropertyOptions{.speed = 1.0F});
            if (committed) {
                fieldVal = static_cast<std::int64_t>(draft);
                return true;
            }
            return false;
        }

        bool operator()(Math::Vec3 &fieldVal) const {
            std::array draft{fieldVal.x, fieldVal.y, fieldVal.z};
            if (const auto edit = Ui::DrawFloat3PropRow(field.name.c_str(), "value", draft, context.theme.fonts); edit.committed) {
                fieldVal = {draft[0], draft[1], draft[2]};
                return true;
            }
            return false;
        }

        bool operator()(const std::string &fieldVal) const {
            Ui::DrawPropRow(field.name.c_str(), fieldVal.c_str(), context.theme.fonts);
            return false;
        }

        template <typename T> bool operator()([[maybe_unused]] const T &) const noexcept {
            return false;
        }
    };

    /** @brief Renders a single dynamic behavior field using typed std::visit dispatch. */
    bool DrawBehaviorField(Gameplay::BehaviorField &field, const std::span<const char *const, 2> enabledEntries,
                           const EditorGuiContext &context);

    /** @brief Result of drawing one attached behavior card. */
    struct BehaviorCardResult {
        Gameplay::BehaviorComponent edited;
        bool committed{false};
        bool removeRequested{false};
    };

    /** @brief Draws one behavior card and reports the requested mutation. */
    [[nodiscard]] inline BehaviorCardResult DrawBehaviorCard(const Gameplay::BehaviorComponent &attached,
                                                             const Gameplay::BehaviorDescriptor *descriptor,
                                                             const std::span<const char *const, 2> enabledEntries,
                                                             const EditorGuiContext &context) {
        const bool missing = descriptor == nullptr;
        const std::string sectionText =
            missing ? context.localization.Get("editor", "workspace.inspector.behavior_missing") + " — " + attached.typeId.Value()
                    : descriptor->displayName;
        BehaviorCardResult result{.edited = attached};
        Ui::Card card(Ui::CardProps{.id = "##BehaviorCard"});
        const ComponentTitleBarResult header =
            DrawComponentTitleBar(card, sectionText.c_str(),
                                  {.enabled = result.edited.enabled, .canReset = false, .canToggleEnabled = true, .canRemove = true},
                                  context);
        result.removeRequested = header.removeRequested;
        if (header.toggleEnabledRequested) {
            result.edited.enabled = !result.edited.enabled;
            result.committed = true;
        }
        if (!card.BeginBody())
            return result;

        ImGui::BeginDisabled(!result.edited.enabled);
        if (int enabled = result.edited.enabled ? 1 : 0;
            Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.behavior_enabled").c_str(), "enabled", enabled,
                                 enabledEntries, context.theme.fonts)) {
            result.edited.enabled = enabled != 0;
            result.committed = true;
        }
        if (!missing) {
            for (Gameplay::BehaviorField &field : result.edited.fields)
                result.committed |= DrawBehaviorField(field, enabledEntries, context);
        }
        ImGui::EndDisabled();
        return result;
    }

    /** @brief Geometry shared by the editable object title and its trailing controls. */
    struct ObjectTitleLayout {
        float uiScale;
        float rowHeight;
        float checkboxSize;
        float checkboxGap;
        float optionsWidth;
        float staticFontSize;
        float trailingWidth;
    };

    /** @brief Resolves object-title geometry from localized text and the active UI scale. */
    [[nodiscard]] inline ObjectTitleLayout ResolveObjectTitleLayout(const char *staticLabel, const Theme::Fonts &fonts) {
        const float uiScale = Theme::GetActiveTokens().sizes.uiScale;
        const float checkboxSize = 14.0F * uiScale;
        const float checkboxGap = 4.0F * uiScale;
        const float optionsWidth = 30.0F * uiScale;
        const float staticFontSize = Theme::TextPx::Label();
        const float staticTextWidth = fonts.sansCompact->CalcTextSizeA(staticFontSize, 1000.0F, 0.0F, staticLabel).x;
        return {.uiScale = uiScale,
                .rowHeight = 38.0F * uiScale,
                .checkboxSize = checkboxSize,
                .checkboxGap = checkboxGap,
                .optionsWidth = optionsWidth,
                .staticFontSize = staticFontSize,
                .trailingWidth = checkboxSize + checkboxGap + staticTextWidth + 6.0F * uiScale + optionsWidth};
    }

    /** @brief Draws the read-only static-object indicator beside the object title. */
    inline void DrawStaticObjectIndicator(const char *label, const Theme::Fonts &fonts, const ObjectTitleLayout &layout,
                                          const ImVec2 rowOrigin, const float rowWidth) {
        ImGui::SetCursorScreenPos(
            {rowOrigin.x + rowWidth - layout.trailingWidth, rowOrigin.y + (layout.rowHeight - layout.checkboxSize) * 0.5F});
        bool isStatic = true;
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {1.5F, 1.5F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {layout.checkboxGap, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::PushStyleColor(ImGuiCol_CheckMark, Theme::Accent());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
        ImGui::BeginDisabled();
        {
            Theme::ScopedTextStyle textStyle(fonts.sansCompact, layout.staticFontSize, Theme::FontPx::SansCompact);
            static_cast<void>(ImGui::Checkbox(label, &isStatic));
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(5);
    }

    /** @brief Draws the object options trigger and dispatches its contextual commands. */
    inline void DrawObjectOptions(const SceneObject &object, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                                  const ObjectTitleLayout &layout, const ImVec2 rowOrigin, const float rowWidth, const char *tooltip) {
        const ImVec2 position{rowOrigin.x + rowWidth - layout.optionsWidth, rowOrigin.y + (layout.rowHeight - layout.optionsWidth) * 0.5F};
        ImGui::SetCursorScreenPos(position);
        ImGui::PushID("object_options");
        const bool pressed = ImGui::InvisibleButton("##button", {layout.optionsWidth, layout.optionsWidth});
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        if (hovered)
            drawList->AddRectFilled(position, {position.x + layout.optionsWidth, position.y + layout.optionsWidth},
                                    Theme::U32(Theme::Hover()), 4.0F);
        const float iconInset = 6.0F * layout.uiScale;
        const float iconSize = 18.0F * layout.uiScale;
        Ui::DrawEditorIcon(drawList, Ui::UiIcon::MoreVertical, {position.x + iconInset, position.y + iconInset}, {iconSize, iconSize},
                           Theme::U32(hovered ? Theme::Text() : Theme::Muted()), context.theme.fonts.icon);
        if (hovered)
            Ui::ShowTooltip(tooltip, &context.theme.fonts);
        if (pressed)
            ImGui::OpenPopup("##menu");
        if (Ui::BeginMenuPopup("##menu")) {
            using enum EditorWorkspaceViewCommand;
            const bool canMutate = !object.effectivelyLocked && command.command == None;
            if (Ui::ContextMenuItem(context.localization.Get("editor", "workspace.hierarchy.duplicate").c_str(), nullptr,
                                    context.theme.fonts, Ui::ContextMenuItemTone::Normal, Ui::UiIconRegistry::Token(Ui::UiIcon::Duplicate),
                                    canMutate)) {
                command.command = DuplicateObject;
                command.objectPayload = object.id;
            }
            if (Ui::ContextMenuItem(context.localization.Get("editor", "workspace.hierarchy.delete").c_str(), nullptr, context.theme.fonts,
                                    Ui::ContextMenuItemTone::Danger, Ui::UiIconRegistry::Token(Ui::UiIcon::Delete), canMutate)) {
                command.command = DeleteObject;
                command.objectPayload = object.id;
            }
            Ui::EndMenuPopup();
        }
        ImGui::PopID();
    }

    /** @brief Draws the component and behavior entries in the Inspector add-component popup. */
    inline void DrawAddComponentMenuItems(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                          EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        const auto selectComponent = [&command, &object](const ComponentType type) {
            command.command = EditorWorkspaceViewCommand::AddComponentToObject;
            command.objectPayload = object.id;
            command.componentTypePayload = type;
        };
        if (!object.components.camera.has_value() &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.inspector.kind.camera") + "###inspector_component_camera")
                                    .c_str(),
                                nullptr, context.theme.fonts))
            selectComponent(ComponentType::Camera);
        if (!object.components.light.has_value() &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.inspector.kind.light") + "###inspector_component_light")
                                    .c_str(),
                                nullptr, context.theme.fonts))
            selectComponent(ComponentType::Light);
        if (!object.components.triggerVolume.has_value() &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.inspector.kind.trigger_volume") +
                                 "###inspector_component_trigger_volume")
                                    .c_str(),
                                nullptr, context.theme.fonts))
            selectComponent(ComponentType::TriggerVolume);
        if (!object.components.audioSource.has_value() &&
            Ui::ContextMenuItem((context.localization.Get("editor", "workspace.inspector.kind.audio_source") +
                                 "###inspector_component_audio_source")
                                    .c_str(),
                                nullptr, context.theme.fonts))
            selectComponent(ComponentType::AudioSource);
        for (const Gameplay::BehaviorDescriptor &descriptor : viewModel.availableBehaviors) {
            const bool alreadyAttached =
                std::ranges::any_of(object.components.behaviors, [&descriptor](const Gameplay::BehaviorComponent &behavior) {
                return behavior.typeId == descriptor.typeId;
            });
            const std::string behaviorLabel = descriptor.displayName + "###inspector_behavior_" + descriptor.typeId.Value();
            if ((!alreadyAttached || descriptor.allowMultiple) &&
                Ui::ContextMenuItem(behaviorLabel.c_str(), nullptr, context.theme.fonts)) {
                command.command = EditorWorkspaceViewCommand::AttachBehaviorToObject;
                command.objectPayload = object.id;
                command.behaviorTypePayload = descriptor.typeId;
            }
        }
    }

    /** @brief Adopts a component-removal command when no earlier Inspector command won the frame. */
    template <typename Edit>
    [[nodiscard]] inline bool TryAdoptComponentRemoval(const Edit &edit, const SceneObject &object, EditorWorkspaceViewCommandData &command,
                                                       const ComponentType type) {
        using enum EditorWorkspaceViewCommand;
        if (!edit.removeRequested || command.command != None)
            return false;
        command.command = RemoveComponentFromObject;
        command.objectPayload = object.id;
        command.componentTypePayload = type;
        return true;
    }

}  // namespace Horo::Editor::InspectorPanelDetail
