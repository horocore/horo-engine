#pragma once
/** @file SphereColliderFields.h @brief Typed SphereColliderFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::PhysicsSphereCollider; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::PhysicsSphereCollider> {
        using Model = Runtime::PhysicsSphereCollider;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::radiusMeters, Control::Number>("workspace.physics.radius", "workspace.physics.radius"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
