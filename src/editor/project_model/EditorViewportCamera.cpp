/** @copydoc EditorViewportCamera.h */

#include "editor/project_model/EditorViewportCamera.h"

#include <cmath>

namespace Horo::Editor {
    /** @copydoc EditorViewportCamera::IsValid */
    bool EditorViewportCamera::IsValid() const noexcept {
        if (Math::TryLookAt(position, target, up).HasError())
            return false;
        if (projection == Runtime::CameraProjection::Perspective)
            return Math::TryPerspective(verticalFovRadians, 1.0F, nearPlane, farPlane, Math::ClipDepthRange::NegativeOneToOne).HasValue();
        if (projection == Runtime::CameraProjection::Orthographic)
            return Math::TryOrthographic(orthographicHeight, 1.0F, nearPlane, farPlane, Math::ClipDepthRange::NegativeOneToOne).HasValue();
        return false;
    }
}  // namespace Horo::Editor
