#include "editor/screens/workspace/panels/accessibility/AccessibilityContrastPanel.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <charconv>
#include <optional>

namespace Horo::Editor {
    namespace {
        /** @brief Parses an entire fixed opaque color draft, including its final terminator. */
        std::optional<Accessibility::SrgbColor> ParseColor(const std::array<char, 8> &draft) noexcept {
            if (draft.front() != '#' || draft.back() != '\0')
                return std::nullopt;
            std::uint32_t packed{};
            const auto parsed = std::from_chars(draft.data() + 1, draft.data() + 7, packed, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != draft.data() + 7)
                return std::nullopt;
            return Accessibility::SrgbColor{((packed >> 16) & 255) / 255.0, ((packed >> 8) & 255) / 255.0, (packed & 255) / 255.0};
        }

        /** @brief Requires an actual opaque normalized color; compositing remains with the caller. */
        bool IsOpaque(const ImVec4 &color) noexcept {
            return color.w == 1.0F && Accessibility::SrgbColor{color.x, color.y, color.z}.IsValid();
        }

    }  // namespace

    /** @copydoc ContrastInspectionDraft::Evaluate */
    Result<Accessibility::TextContrast> ContrastInspectionDraft::Evaluate() const {
        const auto text = ParseColor(foreground);
        const auto surface = ParseColor(background);
        if (!text || !surface)
            return Result<Accessibility::TextContrast>::Failure(MakeError(Accessibility::ContrastErrors::InvalidColor));
        return Accessibility::EvaluateTextContrast(*text, *surface, size);
    }

    /** @copydoc EvaluateOpaqueContrastPair */
    Result<Accessibility::TextContrast> EvaluateOpaqueContrastPair(const ImVec4 &text, const ImVec4 &surface,
                                                                   const Accessibility::ContrastTextSize size) {
        if (!IsOpaque(text) || !IsOpaque(surface))
            return Result<Accessibility::TextContrast>::Failure(MakeError(Accessibility::ContrastErrors::InvalidColor));
        return Accessibility::EvaluateTextContrast({text.x, text.y, text.z}, {surface.x, surface.y, surface.z}, size);
    }

    /** @copydoc AccessibilityContrastPanel::GetId */
    std::string AccessibilityContrastPanel::GetId() const {
        return "horo.accessibility_contrast";
    }

    /** @copydoc AccessibilityContrastPanel::GetDisplayName */
    std::string AccessibilityContrastPanel::GetDisplayName() const {
        return "workspace.accessibility_contrast.title";
    }

    /** @copydoc AccessibilityContrastPanel::GetDefaultDockArea */
    WorkspaceDockArea AccessibilityContrastPanel::GetDefaultDockArea() const {
        return WorkspaceDockArea::Right;
    }

    /** @copydoc AccessibilityContrastPanel::GetObservedEventTypes */
    std::vector<std::string> AccessibilityContrastPanel::GetObservedEventTypes() const {
        return {};
    }

    /** @copydoc AccessibilityContrastPanel::OnAttach */
    void AccessibilityContrastPanel::OnAttach(PanelContext &) {}

    /** @copydoc AccessibilityContrastPanel::OnDetach */
    void AccessibilityContrastPanel::OnDetach() {}

    /** @copydoc AccessibilityContrastPanel::DrawIcon */
    void AccessibilityContrastPanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const ImVec2 center{position.x + size.x * 0.5F, position.y + size.y * 0.5F};
        drawList->AddCircle(center, size.x * 0.3F, color, 20, 2.0F);
        drawList->AddLine({center.x, center.y - size.y * 0.3F}, {center.x, center.y + size.y * 0.3F}, color, 2.0F);
    }

    /** @copydoc AccessibilityContrastPanel::DrawPanel */
    void AccessibilityContrastPanel::DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &,
                                               EditorWorkspaceViewCommandData &, const EditorGuiContext &context) {
        ImGui::BeginChild("##ContrastInspection", size, false, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextWrapped("%s", LocalizedText(context.localization, "scope").c_str());
        const std::array sourceLabels{LocalizedText(context.localization, "custom").c_str(),
                                      LocalizedText(context.localization, "theme").c_str()};
        int source = inspectTheme_ ? 1 : 0;
        if (Ui::ComboControl("##ContrastSource", &source, sourceLabels.data(), static_cast<int>(sourceLabels.size()), context.theme.fonts))
            inspectTheme_ = source == 1;
        if (!inspectTheme_) {
            ImGui::TextUnformatted(LocalizedText(context.localization, "foreground").c_str());
            static_cast<void>(
                Ui::ColorHexControl("##ContrastForeground", draft_.foreground.data(), draft_.foreground.size(), context.theme.fonts));
            ImGui::TextUnformatted(LocalizedText(context.localization, "background").c_str());
            static_cast<void>(
                Ui::ColorHexControl("##ContrastBackground", draft_.background.data(), draft_.background.size(), context.theme.fonts));
        } else {
            ImGui::TextWrapped("%s", LocalizedText(context.localization, "theme_hint").c_str());
        }
        const std::array labels{LocalizedText(context.localization, "standard").c_str(),
                                LocalizedText(context.localization, "large").c_str()};
        int category = static_cast<int>(draft_.size);
        if (Ui::ComboControl("##ContrastTextCategory", &category, labels.data(), static_cast<int>(labels.size()), context.theme.fonts))
            draft_.size = static_cast<Accessibility::ContrastTextSize>(category);
        ImGui::TextWrapped("%s", LocalizedText(context.localization, "large_hint").c_str());
        const auto result = inspectTheme_ ? EvaluateOpaqueContrastPair(Theme::Text(), Theme::Bg0(), draft_.size) : draft_.Evaluate();
        if (result.HasError()) {
            ImGui::TextWrapped("%s", LocalizedText(context.localization, inspectTheme_ ? "needs_composition" : "invalid").c_str());
        } else {
            ImGui::Text("%s: %.6f:1", LocalizedText(context.localization, "ratio").c_str(), result.Value().ratio);
            ImGui::Text("AA: %s", LocalizedText(context.localization, result.Value().meetsAA ? "pass" : "fail").c_str());
            ImGui::Text("AAA: %s", LocalizedText(context.localization, result.Value().meetsAAA ? "pass" : "fail").c_str());
        }
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
