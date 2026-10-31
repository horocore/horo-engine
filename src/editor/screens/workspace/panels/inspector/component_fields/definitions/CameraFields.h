#pragma once
/** @file CameraFields.h @brief Typed CameraFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {
    inline constexpr std::array cameraProjections{Option(Runtime::CameraProjection::Perspective,
                                                         "workspace.inspector.camera_projection_perspective"),
                                                  Option(Runtime::CameraProjection::Orthographic,
                                                         "workspace.inspector.camera_projection_orthographic")};

    /** @brief Static field registry for Runtime::CameraComponent; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::CameraComponent> {
        using Model = Runtime::CameraComponent;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::projection, Control::Choice>("camera_projection", "workspace.inspector.camera_projection")
                .Options(cameraProjections),
            Bind<Model, &Model::verticalFieldOfViewRadians, Control::Number>("camera_field_of_view",
                                                                             "workspace.inspector.camera_field_of_view")
                .Number({.speed = 0.25F, .format = "%.1f°"}, 180.0F / std::numbers::pi_v<float>)
                .Visible(+[](const Model &v) {
            return v.projection == Runtime::CameraProjection::Perspective;
        }).Valid(+[](const Model &v) {
            return std::isfinite(v.verticalFieldOfViewRadians) && v.verticalFieldOfViewRadians > 0 &&
                   v.verticalFieldOfViewRadians < std::numbers::pi_v<float>;
        }),
            Bind<Model, &Model::orthographicHeight, Control::Number>("camera_orthographic_height",
                                                                     "workspace.inspector.camera_orthographic_height")
                .Visible(+[](const Model &v) {
            return v.projection == Runtime::CameraProjection::Orthographic;
        }).Valid(+[](const Model &v) {
            return v.orthographicHeight > 0;
        }),
            Bind<Model, &Model::nearPlane, Control::Number>("camera_near_plane", "workspace.inspector.camera_near_plane")
                .Number({.speed = 0.01F})
                .Valid(+[](const Model &v) {
            return v.nearPlane > 0 && std::isfinite(v.farPlane) && v.farPlane > v.nearPlane;
        }),
            Bind<Model, &Model::farPlane, Control::Number>("camera_far_plane", "workspace.inspector.camera_far_plane")
                .Number({.speed = 1.0F})
                .Valid(+[](const Model &v) {
            return v.farPlane > v.nearPlane;
        }),
        });
    };
}  // namespace Horo::Editor::InspectorFields
