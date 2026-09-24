#pragma once

#include "editor/project_model/EditorViewportCamera.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>
#include <imgui.h>
#include <optional>

namespace Horo::Editor {
    /** @brief Screen-space geometry produced for one transform-gizmo presentation frame. */
    struct TransformGizmoFrameGeometry {
        Math::Vec3 worldPosition;
        std::array<Math::Vec3, 3> worldAxes{};
        std::optional<ImVec2> center;
        std::array<ImVec2, 3> screenDirections{};
        std::array<float, 3> pixelsPerWorldUnit{};
        std::array<bool, 3> projectedAxisVisible{};
        std::array<std::optional<std::array<ImVec2, 4>>, 3> movePlaneCorners{};
        std::optional<int> hoveredAxis;
    };

    /** @brief Inputs required to project, draw, and hit-test one transform gizmo. */
    struct TransformGizmoGeometryRequest {
        const EditorViewportCamera &camera;
        const Math::Mat4 &worldTransform;
        EditorTransformTool tool{EditorTransformTool::Move};
        EditorTransformSpace space{EditorTransformSpace::Local};
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::NegativeOneToOne};
        std::optional<int> activeAxis;
        std::optional<Math::Vec3> activeWorldPosition;
        ImVec2 origin{};
        float width{0.0F};
        float height{0.0F};
        ImVec2 pointer{};
        bool hovered{false};
    };

    /** @brief Inputs required to project a pointer ray onto a gizmo interaction plane. */
    struct TransformGizmoPlaneProjectionRequest {
        const EditorViewportCamera &camera;
        Math::Vec3 center;
        Math::Vec3 normal;
        ImVec2 pointer{};
        ImVec2 origin{};
        float width{0.0F};
        float height{0.0F};
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::NegativeOneToOne};
    };

    /** @brief Projects and draws a transform gizmo, returning geometry used to begin an interaction. */
    [[nodiscard]] Result<TransformGizmoFrameGeometry> DrawTransformGizmoGeometry(ImDrawList &drawList,
                                                                                 const TransformGizmoGeometryRequest &request);

    /**
     * @brief Draws the active rotation drag's position marker on its selected axis ring.
     * @param drawList Overlay draw list receiving the marker.
     * @param camera Current editor viewport camera.
     * @param center Projected gizmo center in screen pixels.
     * @param rotationVector Current direction on the selected rotation plane.
     * @param axis Active X/Y/Z axis index.
     * @return Success or an invalid camera, vector, center, or axis error.
     */
    [[nodiscard]] Result<void> DrawTransformGizmoRotationPin(ImDrawList &drawList, const EditorViewportCamera &camera, ImVec2 center,
                                                             Math::Vec3 rotationVector, int axis);

    /**
     * @brief Draws the active axis rotation angle as a translucent sector and two radial guides.
     * @param drawList Overlay draw list receiving the sector.
     * @param camera Current editor viewport camera.
     * @param center Projected gizmo center in screen pixels.
     * @param worldAxis Active rotation axis in world space.
     * @param startVector Direction at the beginning of the drag.
     * @param currentVector Current direction on the rotation plane.
     * @return Success or a typed error for invalid projection inputs.
     */
    [[nodiscard]] Result<void> DrawTransformGizmoRotationSweep(ImDrawList &drawList, const EditorViewportCamera &camera, ImVec2 center,
                                                               Math::Vec3 worldAxis, Math::Vec3 startVector, Math::Vec3 currentVector);

    /**
     * @brief Reports whether a linear gizmo axis has a stable screen-space arrow direction for the camera.
     * @param camera Camera used to present the gizmo.
     * @param worldAxis Normalizable world-space axis direction.
     * @return True when the axis is sufficiently separated from the camera view direction, or a typed validation failure.
     */
    [[nodiscard]] Result<bool> HasTransformGizmoLinearAxisScreenDirection(const EditorViewportCamera &camera,
                                                                          Math::Vec3 worldAxis) noexcept;

    /**
     * @brief Projects a pointer ray onto a rotation plane and returns its normalized center-relative vector.
     * @return A vector on hit, empty on geometric miss, or the typed camera/ray/plane failure.
     */
    [[nodiscard]] Result<std::optional<Math::Vec3>> ProjectTransformGizmoRotationVector(
        const TransformGizmoPlaneProjectionRequest &request) noexcept;

    /**
     * @brief Projects a viewport pointer onto a gizmo plane in world coordinates.
     * @param request Camera, plane, viewport, and pointer values for the projection.
     * @return World-space point on hit, empty on miss, or a typed projection error.
     */
    [[nodiscard]] Result<std::optional<Math::Vec3>> ProjectTransformGizmoPlanePoint(
        const TransformGizmoPlaneProjectionRequest &request) noexcept;
}  // namespace Horo::Editor
