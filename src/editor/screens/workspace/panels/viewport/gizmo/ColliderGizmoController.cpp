#include "ColliderGizmoController.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/renderer/EditorViewportScene.h"
#include "editor/screens/workspace/panels/viewport/interaction/ViewportInteractionCapture.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc ColliderGizmoController::Reset */
    void ColliderGizmoController::Reset() noexcept {
        gizmo_.Reset();
        baseline_.clear();
        preview_.clear();
        request_.reset();
        diagnostic_.reset();
    }

    /** @copydoc ColliderGizmoController::OnCaptureCancelled */
    void ColliderGizmoController::OnCaptureCancelled() noexcept {
        gizmo_.OnCaptureCancelled();
        baseline_.clear();
        preview_.clear();
        request_.reset();
        diagnostic_.reset();
    }

    /** @copydoc ColliderGizmoController::IsActive */
    bool ColliderGizmoController::IsActive() const noexcept {
        return gizmo_.IsActive();
    }

    /** @copydoc ColliderGizmoController::ConsumePendingCancellation */
    bool ColliderGizmoController::ConsumePendingCancellation() noexcept {
        EditorWorkspaceViewCommandData ignored;
        return gizmo_.ConsumePendingCancellation(ignored);
    }

    /** @copydoc ColliderGizmoController::Adapt */
    void ColliderGizmoController::Adapt(const EditorWorkspaceViewModel &view, const ColliderPresentation &primary) {
        adapter_.viewportCamera = view.viewportCamera;
        adapter_.activeTransformTool = view.activeTransformTool;
        adapter_.activeTransformSpace = view.activeTransformSpace;
        adapter_.primarySelection = primary.target.object;
        adapter_.objects.resize(1);
        adapter_.objects.front().id = primary.target.object;
        adapter_.objects.front().localTransform = primary.localTransform;
        adapter_.objects.front().effectivelyLocked = !primary.editable;
        adapter_.primarySelectionParentWorldTransform = primary.bodyToWorld;
        adapter_.primarySelectionWorldTransform = Math::Multiply(primary.bodyToWorld, primary.localTransform.ToMatrix());
        adapter_.primarySelectionPreviewWorldTransform.reset();
        if (const auto preview = std::ranges::find(preview_, primary.target, &ColliderTransformEdit::target); preview != preview_.end())
            adapter_.primarySelectionPreviewWorldTransform = Math::Multiply(primary.bodyToWorld, preview->transform.ToMatrix());
    }

    /** @copydoc ColliderGizmoController::DrawGuides */
    void ColliderGizmoController::DrawGuides(ImDrawList &drawList, const TransformGizmoDrawContext &context) const {
        if (context.width <= 0 || context.height <= 0)
            return;
        drawList.PushClipRect(context.origin, {context.origin.x + context.width, context.origin.y + context.height}, true);
        for (const auto &committed : context.viewModel.viewportColliders) {
            if (!committed.visible || committed.diagnostic)
                continue;
            auto collider = committed;
            if (const auto preview = std::ranges::find(preview_, collider.target, &ColliderTransformEdit::target);
                preview != preview_.end())
                collider.localTransform = preview->transform;
            const auto wire = BuildColliderWireframe(collider);
            if (wire.HasError())
                continue;
            const bool selected =
                std::ranges::find(context.viewModel.selectedColliders, collider.target) != context.viewModel.selectedColliders.end();
            const ImU32 color = Theme::U32(selected ? Theme::Accent() : Theme::Muted());
            for (std::size_t index = 0; index < wire.Value().count; ++index) {
                const auto project = [&](const Math::Vec3 point) -> std::optional<Math::Vec2> {
                    const auto projected = ProjectEditorViewportPoint(context.viewModel.viewportCamera, point,
                                                                      context.width / context.height, context.depthRange);
                    if (projected.HasError() || !projected.Value())
                        return std::nullopt;
                    const auto pixels = MapEditorViewportPointToPixels(*projected.Value(), {context.origin.x, context.origin.y},
                                                                       {context.width, context.height});
                    return pixels.HasValue() ? std::optional{pixels.Value()} : std::nullopt;
                };
                const auto first = project(wire.Value().segments[index].first);
                const auto second = project(wire.Value().segments[index].second);
                if (first && second)
                    drawList.AddLine({first->x, first->y}, {second->x, second->y}, color, selected ? 2.0F : 1.0F);
            }
        }
        drawList.PopClipRect();
    }

    /** @copydoc ColliderGizmoController::Advance */
    void ColliderGizmoController::Advance(const TransformGizmoDrawContext &context, const EditorWorkspaceViewCommandData &gesture) {
        if (!request_) {
            request_ = ColliderViewportRequest{context.viewModel.physicsContext,
                                               context.viewModel.colliderSelectionRevision,
                                               context.viewModel.selectedColliders,
                                               {}};
            baseline_.clear();
            for (const auto target : request_->selection) {
                const auto child = std::ranges::find(context.viewModel.viewportColliders, target, &ColliderPresentation::target);
                if (child != context.viewModel.viewportColliders.end())
                    baseline_.push_back(*child);
            }
        }
        if (!gesture.transformPayload)
            return;
        const auto evaluated = EvaluateColliderGroupTransform(baseline_, *gesture.transformPayload, context.input.modifiers.control);
        if (evaluated.HasError()) {
            if (!diagnostic_)
                LOG_ERROR("editor.collider_gizmo", "%s", evaluated.ErrorValue().message.c_str());
            diagnostic_ = evaluated.ErrorValue();
            preview_.clear();
            return;
        }
        diagnostic_.reset();
        preview_ = evaluated.Value();
        if (gesture.command == EditorWorkspaceViewCommand::CommitObjectTransform) {
            context.command.command = EditorWorkspaceViewCommand::CommitPhysicsColliders;
            request_->edits = preview_;
            context.command.colliderPayload = std::move(request_);
            baseline_.clear();
            preview_.clear();
            request_.reset();
        }
    }

    /** @copydoc ColliderGizmoController::Draw */
    bool ColliderGizmoController::Draw(ImDrawList &drawList, const TransformGizmoDrawContext &context, ViewportInteractionCapture &capture,
                                       const EditorGuiContext &gui) {
        const auto &view = context.viewModel;
        if (request_ &&
            (request_->context.session != view.physicsContext.session || request_->context.revision != view.physicsContext.revision ||
             request_->context.state != view.physicsContext.state || request_->selectionRevision != view.colliderSelectionRevision ||
             request_->selection != view.selectedColliders || !view.physicsContext.canEdit || view.playState != EditorPlayState::Idle)) {
            capture.Cancel(Input::CaptureCancellationReason::Explicit);
            Reset();
            DrawGuides(drawList, context);
            return true;
        }
        DrawGuides(drawList, context);
        if (view.selectedColliders.empty() || !view.physicsContext.canEdit || view.playState != EditorPlayState::Idle) {
            if (gizmo_.IsActive())
                capture.Cancel(Input::CaptureCancellationReason::Explicit);
            Reset();
            return false;
        }
        const auto primary = std::ranges::find(view.viewportColliders, view.selectedColliders.front(), &ColliderPresentation::target);
        if (primary == view.viewportColliders.end() || !primary->editable || !primary->visible) {
            if (gizmo_.IsActive())
                capture.Cancel(Input::CaptureCancellationReason::Explicit);
            Reset();
            return true;
        }
        Adapt(view, *primary);
        EditorWorkspaceViewCommandData gesture;
        const bool consumed = gizmo_.Draw(drawList,
                                          {.origin = context.origin,
                                           .width = context.width,
                                           .height = context.height,
                                           .hovered = context.hovered,
                                           .input = context.input,
                                           .viewModel = adapter_,
                                           .command = gesture,
                                           .depthRange = context.depthRange},
                                          capture);
        if (gesture.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview) {
            baseline_.clear();
            preview_.clear();
            request_.reset();
            diagnostic_.reset();
        } else if (gizmo_.IsActive() || gesture.command == EditorWorkspaceViewCommand::CommitObjectTransform) {
            Advance(context, gesture);
        } else if (request_) {
            request_.reset();
            baseline_.clear();
            preview_.clear();
        }
        const auto &hint =
            gui.localization.Get("editor", diagnostic_ ? "workspace.physics.gizmo_rejected" : "workspace.physics.gizmo_hint");
        drawList.PushClipRect(context.origin, {context.origin.x + context.width, context.origin.y + context.height}, true);
        drawList.AddText(nullptr, 0, {context.origin.x + 16, context.origin.y + context.height - 52},
                         Theme::U32(diagnostic_ ? Theme::Err() : Theme::Text()), hint.c_str(), nullptr, std::max(1.0F, context.width - 32));
        drawList.PopClipRect();
        if (consumed)
            return true;
        if (!context.hovered || context.input.modifiers.alt || !context.input.State(Input::PointerButton::Primary).pressed)
            return false;
        const auto ray = BuildEditorViewportRay(view.viewportCamera, (context.input.pointer.x - context.origin.x) / context.width,
                                                (context.input.pointer.y - context.origin.y) / context.height,
                                                context.width / context.height, context.depthRange);
        if (ray.HasError())
            return true;
        const auto picked = PickPhysicsCollider(view.viewportColliders, ray.Value());
        if (picked.HasError())
            return true;
        auto selection =
            context.input.modifiers.control || context.input.modifiers.command ? view.selectedColliders : std::vector<ColliderEditTarget>{};
        if (picked.Value()) {
            const auto found = std::ranges::find(selection, *picked.Value());
            if (found == selection.end())
                selection.push_back(*picked.Value());
            else
                selection.erase(found);
        }
        context.command.command = EditorWorkspaceViewCommand::SelectPhysicsColliders;
        context.command.colliderPayload =
            ColliderViewportRequest{view.physicsContext, view.colliderSelectionRevision, std::move(selection), {}};
        return true;
    }
}  // namespace Horo::Editor
