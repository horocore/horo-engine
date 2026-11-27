#pragma once

/** @file AccessibilityContrastPanel.h
 * @brief Editor contrast inspection using the shared display-independent evaluator.
 */
#include "Horo/Accessibility/Contrast.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "editor/accessibility/EditorControlMetadata.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

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

    /** @brief Explicit inspection pairs from the independent token and native style layers. */
    enum class ThemeContrastPair : std::uint8_t {
        PrimaryRoot,
        PrimaryWindow,
        PrimaryPanel,
        PrimaryRaised,
        MutedRoot,
        MutedPanel,
        DimRaised,
        NativeTextWindow,
        NativeTextPopup,
        NativeTextFrame,
        NativeDisabledWindow,
        Count
    };

    /** @brief Evaluates a selected pair from the active theme at call time without caching colors across theme changes.
     * @param pair Explicit token or native style pair; unknown values fail closed.
     * @param size Category selected from actual font metrics.
     * @return Unrounded assessment or InvalidColor when the pair requires unknown composition or is invalid.
     */
    [[nodiscard]] Result<Accessibility::TextContrast> EvaluateThemeContrastPair(ThemeContrastPair pair,
                                                                                Accessibility::ContrastTextSize size);

    /** @brief Bounded opt-in history of focus transitions. It stores no names, text or widget values. */
    class EditorControlFocusHistory final {
    public:
        static constexpr std::size_t Capacity = 12;
        enum class State : std::uint8_t {
            NoFocus,
            Named,
            Unnamed
        };

        struct Entry final {
            std::uint64_t frame{};
            State state{};
        };

        /** @brief Observes a completed frame and its observed focus, if present, once. */
        void Observe(std::uint64_t frame, const Detail::EditorControlMetadataRecord *focused, bool named) noexcept;
        /** @brief Restarts change detection after an incomplete observation, preserving earlier evidence. */
        void BreakContinuity() noexcept;

        /** @brief Returns the number of retained transitions. */
        [[nodiscard]] std::size_t Size() const noexcept {
            return count_;
        }

        /** @brief Reads a retained transition in oldest-to-newest order, or empty for an invalid index. */
        [[nodiscard]] std::optional<Entry> Read(std::size_t index) const noexcept;

    private:
        struct Focus final {
            std::uint32_t window{}, item{};
            State state{};
            bool operator==(const Focus &) const noexcept = default;
        };

        std::array<Entry, Capacity> entries_{};
        std::optional<Focus> previous_;
        std::uint64_t lastFrame_{};
        std::size_t oldest_{}, count_{};
    };

    /** @brief Persistent developer tool for composed sRGB drafts and explicit token/native style pair inspection. */
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
        /** @copydoc IWorkspacePanel::OnPresentationHidden */
        void OnPresentationHidden(EditorWorkspaceViewCommandData &command) override;
        /** @copydoc IWorkspacePanel::DrawIcon */
        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        /** @copydoc IWorkspacePanel::DrawPanel */
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;

    private:
        /** @brief Selects the token or native style colors to evaluate. */
        void DrawThemePairSelection(const EditorGuiContext &context);
        /** @brief Lets the developer inspect the live viewport with a selected color-vision transform. */
        void DrawColorPreview(const EditorGuiContext &context);
        /** @brief Withdraws this panel's transient preview when it leaves presentation. */
        void ResetColorPreview();
        /** @brief Shows opt-in observations from the last full editor control frame. */
        void DrawControlInspection(const EditorGuiContext &context);
        /** @brief Displays a completed control frame or its capture status. */
        void DrawControlInspectionFrame(const EditorGuiContext &context, const Detail::EditorControlMetadataFrame &frame);
        /** @brief Shows the recent focus transitions without persisting any captured text. */
        void DrawControlFocusHistory(const EditorGuiContext &context) const;
        /** @brief Starts the context-local capture, or reports that its owner is unavailable. */
        void StartControlInspection();
        /** @brief Releases captured frame data when the tool stops being presented. */
        void ResetControlInspection() noexcept;
        ContrastInspectionDraft draft_;
        bool inspectTheme_{};
        ThemeContrastPair themePair_{ThemeContrastPair::PrimaryRoot};
        IEditorViewportRenderer *viewport_{};
        int previewSelection_{};
        bool previewRejected_{};
        std::unique_ptr<Detail::EditorControlMetadataCapture> controlCapture_;
        EditorControlFocusHistory focusHistory_;
        bool controlCaptureUnavailable_{};
    };
}  // namespace Horo::Editor
