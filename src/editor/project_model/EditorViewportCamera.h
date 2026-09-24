#pragma once

/**
 * @file EditorViewportCamera.h
 * @brief Backend-neutral editor viewport camera contract owned by the editor model layer.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <cstdint>

namespace Horo::Editor {
    /** @brief Signed world-axis viewpoint selected from the viewport compass. */
    enum class EditorViewportAxisView : std::uint8_t {
        PositiveX,
        NegativeX,
        PositiveY,
        NegativeY,
        PositiveZ,
        NegativeZ,
    };

    /** @brief Immutable perspective or orthographic camera values for one editor viewport render. */
    struct EditorViewportCamera {
        Runtime::CameraProjection projection{Runtime::CameraProjection::Perspective};
        Math::Vec3 position{0.0F, 0.0F, 4.0F};
        Math::Vec3 target{};
        Math::Vec3 up{0.0F, 1.0F, 0.0F};
        float verticalFovRadians{0.9599310885968813F};
        float orthographicHeight{8.0F};
        float nearPlane{0.1F};
        float farPlane{100.0F};

        /** @brief Reports whether the camera can produce a finite view for its selected projection. */
        [[nodiscard]] bool IsValid() const noexcept;
    };
}  // namespace Horo::Editor
