#pragma once

/** @file AccessibilityContrastPanel.h
 * @brief Editor contrast inspection using the shared display-independent evaluator.
 */
#include "Horo/Accessibility/Contrast.h"
#include "Horo/Editor/IWorkspacePanel.h"

#include <array>

struct ImVec4;

namespace Horo::Editor {
    /** @brief Editable opaque sRGB pair; invalid intermediate input remains visible and is never silently substituted. */
    struct ContrastInspectionDraft final {
        std::array<char, 8> foreground{'#', 'f', 'f', 'f', 'f', 'f', 'f', '\0'};
        std::array<char, 8> background{'#', '0', '0', '0', '0', '0', '0', '\0'};
        Accessibility::ContrastTextSize size{Accessibility::ContrastTextSize::Standard};

        /** @brief Parses both exact #RRGGBB drafts and evaluates numeric contrast.
         * @return Unrounded contrast outcomes or InvalidColor; never uses a cached valid color after an invalid edit.
         */
        [[nodiscard]] Result<Accessibility::TextContrast> Evaluate() const;
    };

    /** @brief Evaluates actual unquantized theme colors without inferring an unknown composition.
     * @param text Finite normalized encoded sRGB text, alpha exactly one.
     * @param surface Finite normalized encoded sRGB surface, alpha exactly one.
     * @param size Caller-selected category from actual font metrics.
     * @return Numeric assessment or InvalidColor for unknown composition or invalid channels/category.
     */
    [[nodiscard]] Result<Accessibility::TextContrast> EvaluateOpaqueContrastPair(const ImVec4 &text, const ImVec4 &surface,
                                                                                 Accessibility::ContrastTextSize size);

    /** @brief Persistent developer tool for user-selected composed sRGB colors and active-theme text/window inspection. */
    class AccessibilityContrastPanel final : public IWorkspacePanel {
    public:
        static constexpr GuiLocalizationScope kTextScope{"workspace.accessibility_contrast"};

        /** @brief Creates a tool with an opaque white-on-black draft and standard text thresholds. */
        AccessibilityContrastPanel() : IWorkspacePanel(kTextScope) {}

        /** @copydoc IWorkspacePanel::GetId */
        [[nodiscard]] std::string GetId() const override;
        /** @copydoc IWorkspacePanel::GetDisplayName */
        [[nodiscard]] std::string GetDisplayName() const override;
        /** @copydoc IWorkspacePanel::GetDefaultDockArea */
        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override;
        /** @copydoc IWorkspacePanel::GetObservedEventTypes */
        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override;
        /** @copydoc IWorkspacePanel::OnAttach */
        void OnAttach(PanelContext &context) override;
        /** @copydoc IWorkspacePanel::OnDetach */
        void OnDetach() override;
        /** @copydoc IWorkspacePanel::DrawIcon */
        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        /** @copydoc IWorkspacePanel::DrawPanel */
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;

    private:
        ContrastInspectionDraft draft_;
        bool inspectTheme_{};
    };
}  // namespace Horo::Editor
