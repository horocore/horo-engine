#include "editor/screens/workspace/panels/accessibility/AccessibilityContrastPanel.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "editor/renderer/EditorViewportRenderer.h"

#include <algorithm>
#include <charconv>
#include <new>
#include <optional>
#include <utility>

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

    /** @copydoc EvaluateThemeContrastPair */
    Result<Accessibility::TextContrast> EvaluateThemeContrastPair(const ThemeContrastPair pair,
                                                                  const Accessibility::ContrastTextSize size) {
        switch (pair) {
            case ThemeContrastPair::PrimaryRoot:
                return EvaluateOpaqueContrastPair(Theme::Text(), Theme::Bg0(), size);
            case ThemeContrastPair::PrimaryWindow:
                return EvaluateOpaqueContrastPair(Theme::Text(), Theme::Bg1(), size);
            case ThemeContrastPair::PrimaryPanel:
                return EvaluateOpaqueContrastPair(Theme::Text(), Theme::Bg2(), size);
            case ThemeContrastPair::PrimaryRaised:
                return EvaluateOpaqueContrastPair(Theme::Text(), Theme::Bg3(), size);
            case ThemeContrastPair::MutedRoot:
                return EvaluateOpaqueContrastPair(Theme::Muted(), Theme::Bg0(), size);
            case ThemeContrastPair::MutedPanel:
                return EvaluateOpaqueContrastPair(Theme::Muted(), Theme::Bg2(), size);
            case ThemeContrastPair::DimRaised:
                return EvaluateOpaqueContrastPair(Theme::Dim(), Theme::Bg3(), size);
            case ThemeContrastPair::NativeTextWindow:
                return EvaluateOpaqueContrastPair(ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_WindowBg),
                                                  size);
            case ThemeContrastPair::NativeTextPopup:
                return EvaluateOpaqueContrastPair(ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_PopupBg),
                                                  size);
            case ThemeContrastPair::NativeTextFrame:
                return EvaluateOpaqueContrastPair(ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_FrameBg),
                                                  size);
            case ThemeContrastPair::NativeDisabledWindow:
                return EvaluateOpaqueContrastPair(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                                  ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), size);
            default:
                return Result<Accessibility::TextContrast>::Failure(MakeError(Accessibility::ContrastErrors::InvalidColor));
        }
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
    void AccessibilityContrastPanel::OnAttach(PanelContext &context) {
        viewport_ = context.viewportRenderer;
    }

    /** @copydoc AccessibilityContrastPanel::OnDetach */
    void AccessibilityContrastPanel::OnDetach() {
        ResetColorPreview();
        ResetControlInspection();
        viewport_ = nullptr;
    }

    /** @copydoc AccessibilityContrastPanel::OnPresentationHidden */
    void AccessibilityContrastPanel::OnPresentationHidden(EditorWorkspaceViewCommandData &) {
        ResetColorPreview();
        ResetControlInspection();
    }

    /** @copydoc AccessibilityContrastPanel::ResetColorPreview */
    void AccessibilityContrastPanel::ResetColorPreview() {
        if (viewport_ && previewSelection_ != 0)
            static_cast<void>(viewport_->RequestColorAccessibilityPreview(std::nullopt));
        previewSelection_ = 0;
        previewRejected_ = false;
    }

    /** @copydoc AccessibilityContrastPanel::DrawColorPreview */
    void AccessibilityContrastPanel::DrawColorPreview(const EditorGuiContext &context) {
        if (!viewport_)
            return;
        using enum Accessibility::ColorblindMode;
        constexpr std::array modes{None, Protanopia, Deuteranopia, Tritanopia, Achromatopsia};
        const auto &localization = context.localization;
        const std::array labels{LocalizedText(localization, "preview_off").c_str(),
                                LocalizedText(localization, "preview_protanopia").c_str(),
                                LocalizedText(localization, "preview_deuteranopia").c_str(),
                                LocalizedText(localization, "preview_tritanopia").c_str(),
                                LocalizedText(localization, "preview_achromatopsia").c_str()};
        static_assert(labels.size() == modes.size());
        auto selection = previewSelection_;
        if (Ui::DrawComboPropRow(LocalizedText(localization, "preview_label").c_str(), "##ColorVisionPreview", selection, labels,
                                 context.theme.fonts)) {
            const auto mode = modes[static_cast<std::size_t>(selection)];
            const auto preview = mode == None ? std::nullopt : std::optional{Accessibility::ColorblindSettings{.mode = mode}};
            previewRejected_ = viewport_->RequestColorAccessibilityPreview(preview).HasError();
            if (!previewRejected_)
                previewSelection_ = selection;
        }
        if (previewRejected_)
            ImGui::TextWrapped("%s", LocalizedText(localization, "preview_unavailable").c_str());
    }

    /** @copydoc AccessibilityContrastPanel::StartControlInspection */
    void AccessibilityContrastPanel::StartControlInspection() {
        controlCaptureUnavailable_ = true;
        try {
            auto capture = std::make_unique<Detail::EditorControlMetadataCapture>();
            if (!capture->Bind(ImGui::GetCurrentContext()))
                return;
            controlCapture_ = std::move(capture);
            controlCaptureUnavailable_ = false;
        } catch (const std::bad_alloc &) {
            // The inspection is optional; allocation failure must leave the workspace usable.
        }
    }

    /** @copydoc AccessibilityContrastPanel::ResetControlInspection */
    void AccessibilityContrastPanel::ResetControlInspection() noexcept {
        controlCapture_.reset();
        controlCaptureUnavailable_ = false;
    }

    /** @copydoc AccessibilityContrastPanel::DrawControlInspection */
    void AccessibilityContrastPanel::DrawControlInspection(const EditorGuiContext &context) {
        const auto &localization = context.localization;
        const auto label = std::string{LocalizedText(localization, controlCapture_ ? "controls_stop" : "controls_start").c_str()} +
                           "###ControlInspectionToggle";
        if (Ui::Button({.label = label.c_str(), .font = context.theme.fonts.sans})) {
            if (controlCapture_)
                ResetControlInspection();
            else
                StartControlInspection();
        }
        if (controlCaptureUnavailable_)
            ImGui::TextWrapped("%s", LocalizedText(localization, "controls_unavailable").c_str());
        if (controlCapture_)
            DrawControlInspectionFrame(context, controlCapture_->Frame());
    }

    /** @copydoc AccessibilityContrastPanel::DrawControlInspectionFrame */
    void AccessibilityContrastPanel::DrawControlInspectionFrame(const EditorGuiContext &context,
                                                                const Detail::EditorControlMetadataFrame &frame) const {
        const auto &localization = context.localization;
        using enum Detail::EditorControlMetadataStatus;
        // Binding starts during this panel's draw. The first frame missed controls drawn before it.
        if (frame.status == NoFrame || frame.sequence == 1) {
            ImGui::TextWrapped("%s", LocalizedText(localization, "controls_waiting").c_str());
            return;
        }
        if (frame.status != Complete) {
            ImGui::TextWrapped("%s", LocalizedText(localization, "controls_incomplete").c_str());
            return;
        }

        ImGui::TextWrapped("%s %zu", LocalizedText(localization, "controls_observed").c_str(), frame.controls.size());
        ImGui::TextWrapped("%s %u", LocalizedText(localization, "controls_missing").c_str(), frame.missingNames);
        const auto focused = std::ranges::find_if(frame.controls, [](const Detail::EditorControlMetadataRecord &control) {
            return !control.clipped && control.state.Has(Runtime::Ui::UiAccessibilityStateFlag::Focused);
        });
        if (focused == frame.controls.end())
            return;
        const auto name = frame.Text(focused->name);
        if (name.empty())
            ImGui::TextWrapped("%s", LocalizedText(localization, "controls_focus_unnamed").c_str());
        else
            ImGui::TextWrapped("%s %.*s", LocalizedText(localization, "controls_focus").c_str(), static_cast<int>(name.size()), name.data());
    }

    /** @copydoc AccessibilityContrastPanel::DrawIcon */
    void AccessibilityContrastPanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const ImVec2 center{position.x + size.x * 0.5F, position.y + size.y * 0.5F};
        drawList->AddCircle(center, size.x * 0.3F, color, 20, 2.0F);
        drawList->AddLine({center.x, center.y - size.y * 0.3F}, {center.x, center.y + size.y * 0.3F}, color, 2.0F);
    }

    /** @copydoc AccessibilityContrastPanel::DrawThemePairSelection */
    void AccessibilityContrastPanel::DrawThemePairSelection(const EditorGuiContext &context) {
        const auto &localization = context.localization;
        ImGui::TextWrapped("%s", LocalizedText(localization, "theme_hint").c_str());
        const std::array labels{LocalizedText(localization, "pair_primary_root").c_str(),
                                LocalizedText(localization, "pair_primary_window").c_str(),
                                LocalizedText(localization, "pair_primary_panel").c_str(),
                                LocalizedText(localization, "pair_primary_raised").c_str(),
                                LocalizedText(localization, "pair_muted_root").c_str(),
                                LocalizedText(localization, "pair_muted_panel").c_str(),
                                LocalizedText(localization, "pair_dim_raised").c_str(),
                                LocalizedText(localization, "pair_native_window").c_str(),
                                LocalizedText(localization, "pair_native_popup").c_str(),
                                LocalizedText(localization, "pair_native_frame").c_str(),
                                LocalizedText(localization, "pair_native_disabled").c_str()};
        static_assert(labels.size() == static_cast<std::size_t>(ThemeContrastPair::Count));
        int selection = static_cast<int>(themePair_);
        if (Ui::ComboControl("##ContrastThemePair", &selection, labels.data(), static_cast<int>(labels.size()), context.theme.fonts))
            themePair_ = static_cast<ThemeContrastPair>(selection);
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
            DrawThemePairSelection(context);
        }
        const std::array labels{LocalizedText(context.localization, "standard").c_str(),
                                LocalizedText(context.localization, "large").c_str()};
        int category = static_cast<int>(draft_.size);
        if (Ui::ComboControl("##ContrastTextCategory", &category, labels.data(), static_cast<int>(labels.size()), context.theme.fonts))
            draft_.size = static_cast<Accessibility::ContrastTextSize>(category);
        ImGui::TextWrapped("%s", LocalizedText(context.localization, "large_hint").c_str());
        const auto result = inspectTheme_ ? EvaluateThemeContrastPair(themePair_, draft_.size) : draft_.Evaluate();
        if (result.HasError()) {
            ImGui::TextWrapped("%s", LocalizedText(context.localization, inspectTheme_ ? "needs_composition" : "invalid").c_str());
        } else {
            ImGui::Text("%s: %.6f:1", LocalizedText(context.localization, "ratio").c_str(), result.Value().ratio);
            ImGui::Text("AA: %s", LocalizedText(context.localization, result.Value().meetsAA ? "pass" : "fail").c_str());
            ImGui::Text("AAA: %s", LocalizedText(context.localization, result.Value().meetsAAA ? "pass" : "fail").c_str());
        }
        DrawColorPreview(context);
        ImGui::Separator();
        DrawControlInspection(context);
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
