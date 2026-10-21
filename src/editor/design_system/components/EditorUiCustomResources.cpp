#include "EditorUiCustomResources.h"

#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <imgui_internal.h>
#include <numbers>

namespace Horo::Editor::Ui {
    /** @copydoc ScopedEditorUiInput::ScopedEditorUiInput */
    ScopedEditorUiInput::ScopedEditorUiInput(bool admitted) {
        ImGui::BeginDisabled(!admitted);
    }

    /** @copydoc ScopedEditorUiInput::~ScopedEditorUiInput */
    ScopedEditorUiInput::~ScopedEditorUiInput() {
        ImGui::EndDisabled();
    }

    /** @copydoc ApplyEditorUiInput */
    void ApplyEditorUiInput(Extensions::EditorUiRenderSnapshot &snapshot, bool admitted) noexcept {
        const auto count = std::min(snapshot.nodes.size(), snapshot.accessibility.size());
        for (std::size_t i = 0; i < count; ++i) {
            snapshot.accessibility[i].enabled = admitted && snapshot.nodes[i].enabled;
            snapshot.accessibility[i].focusable = admitted && snapshot.nodes[i].focusable;
            if (!admitted)
                snapshot.accessibility[i].preview = false;
        }
    }

    /** @copydoc EditorUiFonts */
    Theme::Fonts EditorUiFonts(const Theme::Fonts &fonts, Extensions::EditorThemeFontRole role) noexcept {
        auto result = fonts;
        ImFont *selected = fonts.sans;
        if (role == Extensions::EditorThemeFontRole::SansCompact)
            selected = fonts.sansCompact;
        else if (role == Extensions::EditorThemeFontRole::SansEmphasis)
            selected = fonts.sansEmphasis;
        result.sans = selected;
        result.sansCompact = selected;
        result.sansEmphasis = selected;
        return result;
    }

    /** @copydoc EditorUiIcon */
    UiIcon EditorUiIcon(Extensions::EditorThemeIconRole role) noexcept {
        static constexpr std::array icons{UiIcon::None,         UiIcon::Generic,    UiIcon::Info,          UiIcon::Warning,
                                          UiIcon::Error,        UiIcon::Create,     UiIcon::Rename,        UiIcon::Duplicate,
                                          UiIcon::Delete,       UiIcon::Reset,      UiIcon::Check,         UiIcon::Settings,
                                          UiIcon::MoreVertical, UiIcon::Visibility, UiIcon::VisibilityOff, UiIcon::Lock,
                                          UiIcon::Search,       UiIcon::ArrowBack,  UiIcon::ArrowForward,  UiIcon::Folder,
                                          UiIcon::Description,  UiIcon::Play,       UiIcon::Pause,         UiIcon::Stop,
                                          UiIcon::Record};
        const auto index = static_cast<std::size_t>(role);
        return index < icons.size() ? icons[index] : UiIcon::None;
    }

    namespace {
        /** @brief Emits only scoped semantic input; the native host remains the focus/capture authority. */
        Extensions::EditorUiScalarInput ScalarInput(Extensions::EditorUiScalarState &state, bool enabled) {
            using enum Extensions::EditorUiScalarInput;
            if (!enabled || ImGui::IsKeyPressed(ImGuiKey_Escape))
                return Cancel;
            if (ImGui::IsItemFocused()) {
                const auto id = ImGui::GetCurrentContext()->LastItemData.ID;
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow, id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, id);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                    return Decrement;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                    return Increment;
            }
            if (state.editing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                return Commit;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                return Begin;
            if (state.editing && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                return Preview;
            return None;
        }

        /** @brief Uses semantic host tokens for every visible dial affordance, clipped to the assigned node. */
        void PaintScalar(const Extensions::EditorUiNumberNode &node, const Extensions::EditorUiScalarState &state,
                         const Extensions::EditorThemeFrame &theme, const Theme::Fonts &fonts, ImVec2 origin, ImVec2 size, bool enabled) {
            using namespace Extensions;
            const auto color = [&](EditorThemeColorRole role) {
                const auto value = ColorFor(theme, role);
                return ImGui::GetColorU32({value.red, value.green, value.blue, value.alpha});
            };
            const auto &style = *node.base.custom;
            const ImVec2 center{origin.x + size.x * 0.5F, origin.y + size.y * 0.5F};
            const float radius = std::min(size.x, size.y) * (0.30F + state.emphasis * 0.05F);
            const double value = state.editing ? state.preview : std::get<double>(node.value);
            const float phase = static_cast<float>((value - *node.minimum) / (*node.maximum - *node.minimum));
            const float angle = (phase * 2.0F - 1.0F) * std::numbers::pi_v<float>;
            const float stroke = std::max(theme.uiScale, theme.interaction.focusRingWidth * theme.uiScale);
            auto *draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
            draw->AddCircle(center, radius, color(enabled ? style.accent : EditorThemeColorRole::TextDisabled), 32, stroke);
            draw->AddLine(center, {center.x + std::sin(angle) * radius, center.y - std::cos(angle) * radius},
                          color(enabled ? style.accent : EditorThemeColorRole::TextDisabled), stroke);
            const float iconSize = theme.icons.mediumSize * theme.uiScale;
            DrawEditorIcon(draw, EditorUiIcon(ResolveEditorThemeIconRole(style.icon, theme)), {origin.x, origin.y}, {iconSize, iconSize},
                           color(EditorThemeColorRole::TextPrimary), fonts.icon);
            if (ImGui::IsItemFocused())
                draw->AddRect(origin, {origin.x + size.x, origin.y + size.y}, color(EditorThemeColorRole::Focus), 0, 0, stroke);
            draw->PopClipRect();
        }
    }  // namespace

    /** @copydoc EditorUiScalar */
    std::optional<double> EditorUiScalar(const Extensions::EditorUiNumberNode &node, Extensions::EditorUiScalarState &state,
                                         Extensions::EditorUiAccessibleNode *semantic, const Extensions::EditorThemeFrame &theme,
                                         const Theme::Fonts &fonts, bool enabled, ImVec2 size) {
        char text[64]{};
        std::snprintf(text, sizeof(text), "%.9g", state.editing ? state.preview : std::get<double>(node.value));
        Hint(text, fonts);
        const auto origin = ImGui::GetCursorScreenPos();
        size.x = std::max(size.x, theme.uiScale);
        size.y = std::max(size.y, theme.metrics.minimumInteractiveTarget * theme.uiScale);
        static_cast<void>(ImGui::Button("##custom_scalar", size));
        enabled = enabled && (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) == 0;
        const auto input = ScalarInput(state, enabled);
        auto proposed =
            Extensions::UpdateEditorUiScalar(node, state, input, static_cast<double>((ImGui::GetIO().MousePos.x - origin.x) / size.x),
                                             enabled, theme, ImGui::GetIO().DeltaTime);
        PaintScalar(node, state, theme, fonts, origin, size, enabled);
        if (semantic) {
            semantic->enabled = enabled;
            semantic->focusable = enabled && !node.base.readOnly;
            semantic->value = state.editing || proposed ? state.preview : std::get<double>(node.value);
            semantic->preview = state.editing || proposed.has_value();
        }
        return proposed;
    }
}  // namespace Horo::Editor::Ui
