#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "TransformGizmoController.h"

namespace Horo::Editor {
    /** @brief Backend-neutral collider overlay and transient pointer session; the host alone owns document/history edits. */
    class ColliderGizmoController final {
    public:
        /** @brief Discard all pointer-derived state at panel teardown. */
        void Reset() noexcept;
        /** @brief Invalidate a gesture synchronously when capture is lost. */
        void OnCaptureCancelled() noexcept;
        /** @brief Whether this adapter currently owns a pointer gesture. */
        [[nodiscard]] bool IsActive() const noexcept;
        /** @brief Drain a cancelled preview without issuing an authored command. */
        [[nodiscard]] bool ConsumePendingCancellation() noexcept;
        /** @brief Draw collider guides and emit one fenced child-selection or commit request.
         * @param drawList Viewport overlay. @param context Routed logical-pixel frame input.
         * @param capture Exclusive pointer owner. @return Whether input was consumed.
         */
        [[nodiscard]] bool Draw(ImDrawList &drawList, const TransformGizmoDrawContext &context, ViewportInteractionCapture &capture,
                                const EditorGuiContext &gui);

    private:
        /** @brief Build the narrow existing transform-gizmo adapter for a stable collider pivot. */
        void Adapt(const EditorWorkspaceViewModel &view, const ColliderPresentation &primary);
        /** @brief Draw fixed world geometry clipped to this viewport using the active renderer's depth convention. */
        void DrawGuides(ImDrawList &drawList, const TransformGizmoDrawContext &context) const;
        /** @brief Capture and evaluate a transient group preview, producing no document mutation. */
        void Advance(const TransformGizmoDrawContext &context, const EditorWorkspaceViewCommandData &gesture);
        TransformGizmoController gizmo_;
        EditorWorkspaceViewModel adapter_;
        std::vector<ColliderPresentation> baseline_;
        std::vector<ColliderTransformEdit> preview_;
        std::optional<ColliderViewportRequest> request_;
        std::optional<Error> diagnostic_;
    };
}  // namespace Horo::Editor
