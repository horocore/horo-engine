#pragma once
/** @file RigidBodyFields.h @brief Typed RigidBodyFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::RigidBodyComponent; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::RigidBodyComponent> {
        using Model = Runtime::RigidBodyComponent;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::initialLinearVelocity, Control::Vector3>("workspace.physics.linear_velocity",
                                                                         "workspace.physics.linear_velocity"),
            Bind<Model, &Model::initialAngularVelocity, Control::Vector3>("workspace.physics.angular_velocity",
                                                                          "workspace.physics.angular_velocity"),
            Bind<Model, &Model::linearDampingPerSecond, Control::Number>("workspace.physics.linear_damping",
                                                                         "workspace.physics.linear_damping"),
            Bind<Model, &Model::angularDampingPerSecond, Control::Number>("workspace.physics.angular_damping",
                                                                          "workspace.physics.angular_damping"),
            Bind<Model, &Model::maximumLinearSpeed, Control::Number>("workspace.physics.linear_limit", "workspace.physics.linear_limit"),
            Bind<Model, &Model::maximumAngularSpeed, Control::Number>("workspace.physics.angular_limit", "workspace.physics.angular_limit"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
