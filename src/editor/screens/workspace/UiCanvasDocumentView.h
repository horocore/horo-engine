#pragma once

#include "EditorWorkspaceViewModel.h"
#include "Horo/Editor/EditorGuiContext.h"

#include <imgui.h>
#include <memory>
#include <optional>

namespace Horo::Editor {
    class UiCanvasInspectorPanel;

    /** @brief Value-owned Canvas widget adapter; canonical documents and async actions remain with their existing owners. */
    class UiCanvasDocumentView final {
    public:
        /** @brief Borrow the workspace context and own the existing inspector widget for this view lifetime. */
        explicit UiCanvasDocumentView(const EditorGuiContext &context);
        ~UiCanvasDocumentView();

        /** @brief Draw the active immutable Canvas projection and emit typed workspace intents. */
        bool Draw(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &output) const;
        /** @brief Revoke only the held pointer gesture when input admission is cancelled. */
        void CancelGesture() const noexcept;
        /** @brief Clear source-bound widget drafts and gesture state when the source session leaves. */
        void Reset() const noexcept;

    private:
        struct Gesture final {
            DocumentInstanceId document;
            UiCanvasDocumentStateId state;
            Runtime::Ui::UiElementId element;
            std::uint64_t selectionRevision{};
            ImVec2 start;
            ImVec2 viewportExtent;
            ImVec2 origin;
            float scale{};
            UiCanvasPreviewState presentation;
            bool pan{};
        };

        const EditorGuiContext &m_context;
        std::unique_ptr<UiCanvasInspectorPanel> m_inspector;
        mutable std::optional<Gesture> m_gesture;

        /** @brief Present preview controls and copied geometry through the existing document scroll container. */
        void DrawPreview(const UiCanvasWorkspaceDocument &document, EditorWorkspaceViewCommandData &output) const;
        /** @brief Begin an exact source/selection-fenced gesture or emit a bounded guide/selection intent. */
        void HandlePress(const UiCanvasWorkspaceDocument &document, ImVec2 viewportExtent, ImVec2 origin, float scale, bool changed,
                         EditorWorkspaceViewCommandData &output) const;
        /** @brief Finish at captured scale, preserving source and selection publication fences. */
        void FinishGesture(const UiCanvasWorkspaceDocument &document, EditorWorkspaceViewCommandData &output) const;
    };
}  // namespace Horo::Editor
