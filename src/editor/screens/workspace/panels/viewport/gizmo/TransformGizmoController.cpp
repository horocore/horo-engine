#include "TransformGizmoController.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "TransformGizmoGeometry.h"
#include "editor/screens/workspace/panels/viewport/interaction/ViewportInteractionCapture.h"

#include <algorithm>
#include <ranges>

namespace Horo::Editor {
    void TransformGizmoController::OnCaptureCancelled() noexcept {
        if (drag_.has_value())
            cancelPreviewOnNextDraw_ = true;
        drag_.reset();
    }

    void TransformGizmoController::Reset() noexcept {
        drag_.reset();
        cancelPreviewOnNextDraw_ = false;
        geometryFailureReported_ = false;
    }

    bool TransformGizmoController::IsActive() const noexcept {
        return drag_.has_value();
    }

    bool TransformGizmoController::ConsumePendingCancellation(EditorWorkspaceViewCommandData &command) noexcept {
        if (!cancelPreviewOnNextDraw_)
            return false;
        cancelPreviewOnNextDraw_ = false;
        command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
        return true;
    }

    bool TransformGizmoController::Draw(ImDrawList &drawList, const TransformGizmoDrawContext &context,
                                        ViewportInteractionCapture &capture) {
        if (ConsumePendingCancellation(context.command))
            return true;

        const EditorWorkspaceViewModel &viewModel = context.viewModel;
        const auto selectedObject = viewModel.primarySelection.has_value()
                                        ? std::ranges::find(viewModel.objects, *viewModel.primarySelection, &SceneObject::id)
                                        : viewModel.objects.end();
        const bool transformTool = viewModel.activeTransformTool == EditorTransformTool::Move ||
                                   viewModel.activeTransformTool == EditorTransformTool::Rotate ||
                                   viewModel.activeTransformTool == EditorTransformTool::Scale;
        if (drag_.has_value() && (selectedObject == viewModel.objects.end() || selectedObject->id != drag_->object || !transformTool ||
                                  selectedObject->effectivelyLocked || viewModel.activeTransformTool != drag_->tool ||
                                  viewModel.activeTransformSpace != drag_->space)) {
            capture.Cancel(Input::CaptureCancellationReason::Explicit);
            cancelPreviewOnNextDraw_ = false;
            context.command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
            return true;
        }

        if (transformTool && selectedObject != viewModel.objects.end() && !selectedObject->effectivelyLocked &&
            viewModel.primarySelectionWorldTransform.has_value() && viewModel.primarySelectionParentWorldTransform.has_value()) {
            const Math::Mat4 &worldTransform = viewModel.primarySelectionPreviewWorldTransform.has_value()
                                                   ? *viewModel.primarySelectionPreviewWorldTransform
                                                   : *viewModel.primarySelectionWorldTransform;
            Result<TransformGizmoFrameGeometry> geometry =
                DrawTransformGizmoGeometry(drawList, TransformGizmoGeometryRequest{
                                                         .camera = viewModel.viewportCamera,
                                                         .worldTransform = worldTransform,
                                                         .tool = viewModel.activeTransformTool,
                                                         .space = viewModel.activeTransformSpace,
                                                         .depthRange = context.depthRange,
                                                         .activeAxis = drag_.has_value() ? std::optional{drag_->axis} : std::nullopt,
                                                         .activeWorldPosition =
                                                             drag_.has_value() ? std::optional{drag_->currentWorldPosition} : std::nullopt,
                                                         .origin = context.origin,
                                                         .width = context.width,
                                                         .height = context.height,
                                                         .pointer = ImVec2{context.input.pointer.x, context.input.pointer.y},
                                                         .hovered = context.hovered,
                                                     });
            if (geometry.HasError()) {
                if (!geometryFailureReported_) {
                    LOG_ERROR("editor.viewport_gizmo", "Gizmo geometry failed: %s", geometry.ErrorValue().message.c_str());
                    geometryFailureReported_ = true;
                }
                if (drag_.has_value()) {
                    capture.Cancel(Input::CaptureCancellationReason::Explicit);
                    cancelPreviewOnNextDraw_ = false;
                    context.command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
                    return true;
                }
                return false;
            }
            geometryFailureReported_ = false;
            const Result<void> begun = TryBeginDrag(geometry.Value(), worldTransform, *selectedObject, context, capture);
            if (begun.HasError()) {
                LOG_ERROR("editor.viewport_gizmo", "Gizmo drag rejected: %s", begun.ErrorValue().message.c_str());
            }
        }

        if (!drag_.has_value())
            return false;
        AdvanceDrag(context, capture);
        return true;
    }

    Result<void> TransformGizmoController::TryBeginDrag(const TransformGizmoFrameGeometry &geometry, const Math::Mat4 &worldTransform,
                                                        const SceneObject &selectedObject, const TransformGizmoDrawContext &context,
                                                        ViewportInteractionCapture &capture) {
        const Input::RawInputSnapshot &input = context.input;
        const EditorWorkspaceViewModel &viewModel = context.viewModel;
        if (drag_.has_value() || !geometry.hoveredAxis.has_value() || !input.State(Input::PointerButton::Primary).pressed ||
            !capture.Begin(Input::PointerButton::Primary))
            return Result<void>::Success();

        const int axis = *geometry.hoveredAxis;
        const Math::Vec3 chosenAxis = axis < 3 ? geometry.worldAxes[axis] : Math::Vec3{};
        const ImVec2 direction = axis < 3 ? geometry.screenDirections[axis] : ImVec2{0.7071F, -0.7071F};
        const ImVec2 pointer{input.pointer.x, input.pointer.y};
        std::optional<Math::Vec3> startRotationVector;
        if (viewModel.activeTransformTool == EditorTransformTool::Rotate) {
            const Result<std::optional<Math::Vec3>> projected = ProjectTransformGizmoRotationVector({.camera = viewModel.viewportCamera,
                                                                                                     .center = geometry.worldPosition,
                                                                                                     .normal = chosenAxis,
                                                                                                     .pointer = pointer,
                                                                                                     .origin = context.origin,
                                                                                                     .width = context.width,
                                                                                                     .height = context.height,
                                                                                                     .depthRange = context.depthRange});
            if (projected.HasError()) {
                capture.Finish();
                return Result<void>::Failure(projected.ErrorValue());
            }
            if (!projected.Value().has_value()) {
                capture.Finish();
                return Result<void>::Success();
            }
            startRotationVector = projected.Value();
        }

        Result<TransformGizmoMathSession> math = BeginTransformGizmoMath(BeginTransformGizmoMathRequest{
            .tool = viewModel.activeTransformTool,
            .space = viewModel.activeTransformSpace,
            .axis = axis,
            .initialLocalTransform = selectedObject.localTransform,
            .initialWorldTransform = worldTransform,
            .parentWorldTransform = *viewModel.primarySelectionParentWorldTransform,
            .worldAxis = chosenAxis,
            .startRotationVector = startRotationVector,
            .pixelsPerWorldUnit = axis < 3 ? std::max(geometry.pixelsPerWorldUnit[axis], 1.0F) : 1.0F,
        });
        if (math.HasError()) {
            capture.Finish();
            return Result<void>::Failure(math.ErrorValue());
        }

        drag_ = DragSession{
            .object = selectedObject.id,
            .tool = viewModel.activeTransformTool,
            .space = viewModel.activeTransformSpace,
            .axis = axis,
            .draftTransform = selectedObject.localTransform,
            .math = std::move(math).Value(),
            .currentWorldPosition = geometry.worldPosition,
            .startMouse = pointer,
            .screenDirection = direction,
        };
        return Result<void>::Success();
    }

    void TransformGizmoController::AdvanceDrag(const TransformGizmoDrawContext &context, ViewportInteractionCapture &capture) {
        const Input::RawInputSnapshot &input = context.input;
        const Input::ButtonState primary = input.State(Input::PointerButton::Primary);
        if (input.State(Input::Key::Escape).pressed) {
            capture.Cancel(Input::CaptureCancellationReason::Escape);
            context.command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
            cancelPreviewOnNextDraw_ = false;
            return;
        }
        if (primary.released) {
            if (drag_->draftTransform != drag_->math.initialLocalTransform) {
                context.command.command = EditorWorkspaceViewCommand::CommitObjectTransform;
                context.command.objectPayload = drag_->object;
                context.command.transformPayload = drag_->draftTransform;
            }
            drag_.reset();
            capture.Finish();
            return;
        }
        if (!primary.down)
            return;

        const ImVec2 pointer{input.pointer.x, input.pointer.y};
        const ImVec2 mouseDelta{pointer.x - drag_->startMouse.x, pointer.y - drag_->startMouse.y};
        const float projectedPixels = mouseDelta.x * drag_->screenDirection.x + mouseDelta.y * drag_->screenDirection.y;
        std::optional<Math::Vec3> currentRotationVector;
        if (drag_->tool == EditorTransformTool::Rotate) {
            const Result<std::optional<Math::Vec3>> projected =
                ProjectTransformGizmoRotationVector({.camera = context.viewModel.viewportCamera,
                                                     .center = drag_->math.initialWorldPosition,
                                                     .normal = drag_->math.worldAxis,
                                                     .pointer = pointer,
                                                     .origin = context.origin,
                                                     .width = context.width,
                                                     .height = context.height,
                                                     .depthRange = context.depthRange});
            if (projected.HasError()) {
                LOG_ERROR("editor.viewport_gizmo", "Gizmo rotation projection failed: %s", projected.ErrorValue().message.c_str());
                capture.Cancel(Input::CaptureCancellationReason::Explicit);
                cancelPreviewOnNextDraw_ = false;
                context.command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
                return;
            }
            if (!projected.Value().has_value())
                return;
            currentRotationVector = projected.Value();
        }

        Result<TransformGizmoMathOutcome> outcome =
            EvaluateTransformGizmoMath(drag_->math, TransformGizmoMathUpdate{
                                                        .projectedPixels = projectedPixels,
                                                        .currentRotationVector = currentRotationVector,
                                                    });
        if (outcome.HasError()) {
            LOG_ERROR("editor.viewport_gizmo", "Gizmo update failed: %s", outcome.ErrorValue().message.c_str());
            capture.Cancel(Input::CaptureCancellationReason::Explicit);
            cancelPreviewOnNextDraw_ = false;
            context.command.command = EditorWorkspaceViewCommand::CancelObjectTransformPreview;
            return;
        }

        drag_->currentWorldPosition = outcome.Value().worldPosition;
        if (outcome.Value().localTransform != drag_->draftTransform) {
            drag_->draftTransform = outcome.Value().localTransform;
            context.command.command = EditorWorkspaceViewCommand::PreviewObjectTransform;
            context.command.objectPayload = drag_->object;
            context.command.transformPayload = outcome.Value().localTransform;
        }
    }
}  // namespace Horo::Editor
