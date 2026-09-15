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

    /** @brief Inputs required to project a pointer ray onto a gizmo rotation plane. */
    struct TransformGizmoRotationProjectionRequest {
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
     * @brief Projects a pointer ray onto a rotation plane and returns its normalized center-relative vector.
     * @return A vector on hit, empty on geometric miss, or the typed camera/ray/plane failure.
     */
    [[nodiscard]] Result<std::optional<Math::Vec3>> ProjectTransformGizmoRotationVector(
        const TransformGizmoRotationProjectionRequest &request) noexcept;
}  // namespace Horo::Editor
