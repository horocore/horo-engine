#pragma once
/** @file DistanceJointFields.h @brief Typed DistanceJointFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::PhysicsDistanceConstraint; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::PhysicsDistanceConstraint> {
        using Model = Runtime::PhysicsDistanceConstraint;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::minimumMeters, Control::Number>("workspace.physics.minimum_distance", "workspace.physics.minimum_distance"),
            Bind<Model, &Model::maximumMeters, Control::Number>("workspace.physics.maximum_distance", "workspace.physics.maximum_distance"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
