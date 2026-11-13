#pragma once
/** @file CharacterMovementFields.h @brief Typed CharacterMovementFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/CharacterSceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {
    inline constexpr std::array slopePolicies{Option(Character::CharacterSteepSlopePolicy::Stop, "workspace.inspector.character.stop"),
                                              Option(Character::CharacterSteepSlopePolicy::Slide, "workspace.inspector.character.slide")};
    inline constexpr std::array leavePolicies{Option(Character::CharacterPlatformLeaveVelocityPolicy::None,
                                                     "workspace.inspector.character.none"),
                                              Option(Character::CharacterPlatformLeaveVelocityPolicy::Linear,
                                                     "workspace.inspector.character.linear"),
                                              Option(Character::CharacterPlatformLeaveVelocityPolicy::Point,
                                                     "workspace.inspector.character.point")};

    /** @brief Static field registry for Runtime::CharacterSceneMovement; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::CharacterSceneMovement> {
        using Model = Runtime::CharacterSceneMovement;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::skinWidthMeters, Control::Number>("workspace.inspector.character.skin",
                                                                  "workspace.inspector.character.skin"),
            Bind<Model, &Model::minimumMoveDistanceMeters, Control::Number>("workspace.inspector.character.minimum_move",
                                                                            "workspace.inspector.character.minimum_move"),
            Bind<Model, &Model::maximumStepHeightMeters, Control::Number>("workspace.inspector.character.step",
                                                                          "workspace.inspector.character.step"),
            Bind<Model, &Model::maximumSlopeDegrees, Control::Number>("workspace.inspector.character.slope",
                                                                      "workspace.inspector.character.slope"),
            Bind<Model, &Model::jumpSpeedMetersPerSecond, Control::Number>("workspace.inspector.character.jump",
                                                                           "workspace.inspector.character.jump"),
            Bind<Model, &Model::preserveHorizontalSpeed, Control::Boolean>("workspace.inspector.character.preserve_speed",
                                                                           "workspace.inspector.character.preserve_speed"),
            Bind<Model, &Model::dynamicPlatformSupport, Control::Boolean>("workspace.inspector.character.dynamic_platform",
                                                                          "workspace.inspector.character.dynamic_platform"),
            Bind<Model, &Model::inheritPlatformHeading, Control::Boolean>("workspace.inspector.character.platform_heading",
                                                                          "workspace.inspector.character.platform_heading"),
            Bind<Model, &Model::steepSlopePolicy, Control::Choice>("steep_policy", "workspace.inspector.character.steep_policy")
                .Options(slopePolicies),
            Bind<Model, &Model::platformLeaveVelocity, Control::Choice>("leave_velocity", "workspace.inspector.character.leave_velocity")
                .Options(leavePolicies),
        });
    };
}  // namespace Horo::Editor::InspectorFields
