#pragma once

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "editor/project_model/EditorTransformTool.h"

#include <array>
#include <optional>

namespace Horo::Editor {
    namespace TransformGizmoErrors {
        extern const ErrorCodeDescriptor InvalidRequest;
        extern const ErrorCodeDescriptor InvalidUpdate;
        extern const ErrorCodeDescriptor RotationVectorRequired;
    }  // namespace TransformGizmoErrors

    /** @brief Validated, UI-independent state required to evaluate one transform-gizmo drag. */
    struct TransformGizmoMathSession {
        EditorTransformTool tool{EditorTransformTool::Move};
        EditorTransformSpace space{EditorTransformSpace::Local};
        int axis{0};
        Math::Transform initialLocalTransform;
        Math::Mat4 initialWorldTransform{Math::Mat4::Identity()};
        Math::Mat4 parentWorldInverse{Math::Mat4::Identity()};
        Math::Vec3 initialWorldPosition;
        Math::Vec3 worldAxis;
        Math::Vec3 startRotationVector;
        float pixelsPerWorldUnit{1.0F};
    };

    /** @brief Boundary values used to validate and begin one transform-gizmo math session. */
    struct BeginTransformGizmoMathRequest {
        EditorTransformTool tool{EditorTransformTool::Move};
        EditorTransformSpace space{EditorTransformSpace::Local};
        int axis{0};
        Math::Transform initialLocalTransform;
        Math::Mat4 initialWorldTransform{Math::Mat4::Identity()};
        Math::Mat4 parentWorldTransform{Math::Mat4::Identity()};
        Math::Vec3 worldAxis;
        std::optional<Math::Vec3> startRotationVector;
        float pixelsPerWorldUnit{1.0F};
    };

    /** @brief Pointer-derived values used to evaluate a validated gizmo session. */
    struct TransformGizmoMathUpdate {
        float projectedPixels{0.0F};
        std::optional<Math::Vec3> currentRotationVector;
        std::optional<Math::Vec3> worldTranslation; /**< Pointer displacement along an active move plane. */
    };

    /** @brief Draft local transform and world-space gizmo position produced by one evaluation. */
    struct TransformGizmoMathOutcome {
        Math::Transform localTransform;
        Math::Vec3 worldPosition;
    };

    /**
     * @brief Validates transform, axis, scale, rotation, and parent-inverse inputs for a gizmo drag.
     * @param request Boundary values captured when the pointer interaction begins.
     * @return Validated session, or a typed request/math failure.
     */
    [[nodiscard]] Result<TransformGizmoMathSession> BeginTransformGizmoMath(const BeginTransformGizmoMathRequest &request);

    /**
     * @brief Evaluates one move, rotate, or scale update without GUI or document mutation.
     * @param session Validated session returned by BeginTransformGizmoMath.
     * @param update Pointer-derived scalar and optional rotation-plane vector.
     * @return Draft transform, or a typed update/math failure.
     */
    [[nodiscard]] Result<TransformGizmoMathOutcome> EvaluateTransformGizmoMath(const TransformGizmoMathSession &session,
                                                                               const TransformGizmoMathUpdate &update);

    /**
     * @brief Resolves normalized world-space gizmo axes from a validated affine transform.
     * @param worldTransform Current object-to-world transform.
     * @param space Requested local or world orientation.
     * @return Three normalized axes, or the underlying typed decomposition/rotation failure.
     */
    [[nodiscard]] Result<std::array<Math::Vec3, 3>> ResolveTransformGizmoWorldAxes(const Math::Mat4 &worldTransform,
                                                                                   EditorTransformSpace space);
}  // namespace Horo::Editor
