#pragma once

/** @file UiInspectorWidgets.h
 * @brief Shared editor controls for existing typed Runtime UI values; no document mutation.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "UiInspectorDraft.h"

namespace Horo::Editor::UiInspectorWidgets {
    /** @brief Fixed text retained by the host alongside a selected layout/style draft.
     * @details Capture on source/property changes and Cancel. Steady Draw calls retain invalid input.
     */
    struct Buffers final {
        std::array<UiInspectorInteger, 160> integers;
        std::array<char, 37> asset{};
        bool assetValid{true};
        /** @brief Checks every retained field without reparsing. @return Whether text allows Apply. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Capture exact text for sizing, box model, anchors, flex and bounded grid tracks.
     * @param value Typed baseline. @param buffers Caller-owned fixed text storage.
     */
    void CaptureLayout(const Runtime::Ui::UiLayoutStyle &value, Buffers &buffers);
    /** @brief Draw canonical layout sizing, box model, anchors, flex and bounded grid policies.
     * @param context Shared fonts/theme/localization. @param value Detached layout draft.
     * @param buffers Retained text from CaptureLayout. @return Whether input changed, including invalid text.
     */
    bool Layout(const EditorGuiContext &context, Runtime::Ui::UiLayoutStyle &value, Buffers &buffers);
    /** @brief Capture exact text for any of the eight canonical literal categories.
     * @param value Typed baseline. @param buffers Separate caller-owned fixed text storage.
     */
    void CaptureStyle(const Runtime::Ui::UiStyleValue &value, Buffers &buffers);
    /** @brief Draw all fields of the existing literal category without inventing a property schema.
     * @param context Shared fonts/theme/localization. @param value Detached typed literal.
     * @param buffers Retained text from CaptureStyle. @return Whether input changed, including invalid text.
     * @pre The caller scopes widget IDs to the exact selected element/property.
     * @details Registry category/range validation remains with UiInspectorDraft::Prepare and the host command.
     */
    bool Style(const EditorGuiContext &context, Runtime::Ui::UiStyleValue &value, Buffers &buffers);
}  // namespace Horo::Editor::UiInspectorWidgets
