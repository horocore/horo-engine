#pragma once
/** @file CapsuleColliderFields.h @brief Typed CapsuleColliderFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::PhysicsCapsuleCollider; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::PhysicsCapsuleCollider> {
        using Model = Runtime::PhysicsCapsuleCollider;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::radiusMeters, Control::Number>("workspace.physics.radius", "workspace.physics.radius"),
            Bind<Model, &Model::cylindricalHalfHeightMeters, Control::Number>("workspace.physics.cylinder_half_height",
                                                                              "workspace.physics.cylinder_half_height"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
