#pragma once
/** @file TriggerVolumeFields.h @brief Typed TriggerVolumeFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {
    inline constexpr std::array triggerShapes{Option(Runtime::ColliderShapeType::Box, "workspace.inspector.trigger_volume_shape_box"),
                                              Option(Runtime::ColliderShapeType::Sphere, "workspace.inspector.trigger_volume_shape_sphere"),
                                              Option(Runtime::ColliderShapeType::Capsule,
                                                     "workspace.inspector.trigger_volume_shape_capsule"),
                                              Option(Runtime::ColliderShapeType::StaticPlane,
                                                     "workspace.inspector.trigger_volume_shape_plane")};

    /** @brief Static field registry for Runtime::TriggerVolumeComponent; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::TriggerVolumeComponent> {
        using Model = Runtime::TriggerVolumeComponent;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::shape, Control::Choice>("trigger_volume_shape", "workspace.inspector.trigger_volume_shape")
                .Options(triggerShapes),
        });
    };
}  // namespace Horo::Editor::InspectorFields
