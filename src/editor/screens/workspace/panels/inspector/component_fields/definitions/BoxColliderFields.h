#pragma once
/** @file BoxColliderFields.h @brief Typed BoxColliderFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::PhysicsBoxCollider; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::PhysicsBoxCollider> {
        using Model = Runtime::PhysicsBoxCollider;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::halfExtentsMeters, Control::Vector3>("workspace.physics.half_extents", "workspace.physics.half_extents"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
