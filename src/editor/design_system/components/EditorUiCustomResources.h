#pragma once
/** @file EditorUiCustomResources.h @brief Private host adapters for semantic custom composition resources and scalar gestures. */
#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Extensions/EditorUiForm.h"

namespace Horo::Editor::Ui {
    /** @brief Applies the actual workspace input decision to native controls, including custom presenters. */
    class ScopedEditorUiInput final {
    public:
        /** @brief Begins a modal-aware native input scope. @param admitted Whether the workspace owns input. */
        explicit ScopedEditorUiInput(bool admitted);
        /** @brief Restores the enclosing native interaction state. */
        ~ScopedEditorUiInput();
        ScopedEditorUiInput(const ScopedEditorUiInput &) = delete;
        ScopedEditorUiInput &operator=(const ScopedEditorUiInput &) = delete;
    };

    /** @brief Applies current host input ownership to the cached semantic hooks without allocating.
     * @param snapshot Canonical host projection. @param admitted Current owner/modal admission.
     */
    void ApplyEditorUiInput(Extensions::EditorUiRenderSnapshot &snapshot, bool admitted) noexcept;
    /** @brief Borrows only existing host font resources for one resolved role.
     * @param fonts Owning editor font set. @param role Resolved semantic role. @return Frame-borrowed font-role view.
     */
    [[nodiscard]] Theme::Fonts EditorUiFonts(const Theme::Fonts &fonts, Extensions::EditorThemeFontRole role) noexcept;
    /** @brief Resolves an extension semantic icon to the one shared host registry.
     * @param role Resolved semantic identity. @return Existing host icon; never a provider resource.
     */
    [[nodiscard]] UiIcon EditorUiIcon(Extensions::EditorThemeIconRole role) noexcept;
    /** @brief Draws a clipped semantic scalar presenter and proposes a value only on a scoped commit.
     * @param node Validated decimal dial. @param state Host-owned local draft/animation.
     * @param semantic Preallocated accessibility record, or null. @param theme Immutable host frame.
     * @param fonts Existing host fonts. @param enabled Inherited host gate. @param size Allocated content bounds.
     * @return Proposed scalar; the caller must use its canonical approved command/undo route.
     */
    [[nodiscard]] std::optional<double> EditorUiScalar(const Extensions::EditorUiNumberNode &node, Extensions::EditorUiScalarState &state,
                                                       Extensions::EditorUiAccessibleNode *semantic,
                                                       const Extensions::EditorThemeFrame &theme, const Theme::Fonts &fonts, bool enabled,
                                                       ImVec2 size);
}  // namespace Horo::Editor::Ui
