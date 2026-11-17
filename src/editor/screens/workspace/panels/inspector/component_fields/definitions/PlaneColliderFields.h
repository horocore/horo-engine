#pragma once
/** @file PlaneColliderFields.h @brief Typed PlaneColliderFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::PhysicsStaticPlaneCollider; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::PhysicsStaticPlaneCollider> {
        using Model = Runtime::PhysicsStaticPlaneCollider;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::normal, Control::Vector3>("workspace.physics.plane_normal", "workspace.physics.plane_normal"),
            Bind<Model, &Model::signedDistanceMeters, Control::Number>("workspace.physics.plane_distance",
                                                                       "workspace.physics.plane_distance"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
