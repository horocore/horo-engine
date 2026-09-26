#pragma once

#include "editor/screens/workspace/panels/viewport/gizmo/TransformGizmoController.h"
#include "editor/screens/workspace/panels/viewport/interaction/ViewportInteractionCapture.h"
#include "editor/screens/workspace/panels/viewport/navigation/ViewportNavigationController.h"

namespace Horo::Editor {
    /** @brief Frame-local inputs used to coordinate viewport pointer interactions. */
    struct ViewportInteractionDrawContext {
        ImDrawList &drawList;
        ImVec2 origin{};
        float width{0.0F};
        float height{0.0F};
        bool hovered{false};
        const EditorWorkspaceViewModel &viewModel;
        EditorWorkspaceViewCommandData &command;
        const EditorGuiContext &gui;
        float deltaSeconds{0.0F};
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::NegativeOneToOne};
    };

    /**
     * @brief Coordinates mutually exclusive viewport navigation and transform-gizmo pointer interactions.
     *
     * This object owns transient input sessions only. It emits typed workspace commands and never mutates scene or
     * viewport authorities directly.
     */
    class ViewportInteractionController final : public IViewportCaptureCancellationSink {
    public:
        ViewportInteractionController() noexcept;

        void Attach(Input::InputRouter &router, Input::InputContextToken &workspaceContext) noexcept;
        void Detach() noexcept;

        [[nodiscard]] bool IsActive() const noexcept;
        /** @brief Emits an already-queued cancellation while routed input is blocked. */
        [[nodiscard]] bool ConsumePendingCancellation(EditorWorkspaceViewCommandData &command) noexcept;

        void Draw(const ViewportInteractionDrawContext &context);

        void OnViewportCaptureCancelled(Input::CaptureCancellationReason reason) noexcept override;

    private:
        ViewportInteractionCapture capture_;
        ViewportNavigationController navigation_;
        TransformGizmoController gizmo_;
    };
}  // namespace Horo::Editor
