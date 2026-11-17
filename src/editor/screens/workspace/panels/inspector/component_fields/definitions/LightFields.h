#pragma once
/** @file LightFields.h @brief Typed LightFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {
    inline constexpr std::array lightKinds{Option(Runtime::LightKind::Directional, "workspace.inspector.light_kind_directional"),
                                           Option(Runtime::LightKind::Point, "workspace.inspector.light_kind_point"),
                                           Option(Runtime::LightKind::Spot, "workspace.inspector.light_kind_spot")};

    /** @brief Static field registry for Runtime::LightComponent; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::LightComponent> {
        using Model = Runtime::LightComponent;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::kind, Control::Choice>("light_kind", "workspace.inspector.light_kind").Options(lightKinds),
            Bind<Model, &Model::color, Control::Color3>("light_color", "workspace.inspector.light_color").Valid(+[](const Model &v) {
            return std::isfinite(v.color.x) && std::isfinite(v.color.y) && std::isfinite(v.color.z);
        }),
            Bind<Model, &Model::intensity, Control::Number>("light_intensity", "workspace.inspector.light_intensity")
                .Valid(+[](const Model &v) {
            return v.intensity >= 0;
        }),
            Bind<Model, &Model::range, Control::Number>("light_range", "workspace.inspector.light_range")
                .Number({.speed = 0.1F})
                .Visible(+[](const Model &v) {
            return v.kind != Runtime::LightKind::Directional;
        }).Valid(+[](const Model &v) {
            return v.range >= 0;
        }),
            Bind<Model, &Model::innerConeRadians, Control::Number>("light_inner_cone", "workspace.inspector.light_inner_cone")
                .Number({.speed = 0.25F, .format = "%.1f°"}, 180.0F / std::numbers::pi_v<float>)
                .Visible(+[](const Model &v) {
            return v.kind == Runtime::LightKind::Spot;
        }).Valid(+[](const Model &v) {
            return std::isfinite(v.innerConeRadians) && std::isfinite(v.outerConeRadians) && v.innerConeRadians >= 0 &&
                   v.outerConeRadians >= v.innerConeRadians;
        }),
            Bind<Model, &Model::outerConeRadians, Control::Number>("light_outer_cone", "workspace.inspector.light_outer_cone")
                .Number({.speed = 0.25F, .format = "%.1f°"}, 180.0F / std::numbers::pi_v<float>)
                .Visible(+[](const Model &v) {
            return v.kind == Runtime::LightKind::Spot;
        }).Valid(+[](const Model &v) {
            return std::isfinite(v.innerConeRadians) && std::isfinite(v.outerConeRadians) && v.innerConeRadians >= 0 &&
                   v.outerConeRadians >= v.innerConeRadians;
        }),
        });
    };
}  // namespace Horo::Editor::InspectorFields
