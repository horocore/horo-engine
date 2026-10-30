#pragma once
/** @file CharacterGeometryFields.h @brief Typed Character geometry fields in canonical world units. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/CharacterSceneComponents.h"

namespace Horo::Editor::InspectorFields {
    /** @brief Geometry metadata; optional crouch creation remains an explicit typed transition. */
    template <> struct Schema<Runtime::CharacterSceneGeometry> {
        using Model = Runtime::CharacterSceneGeometry;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model,
                 +[](Model &v) -> float & {
            return v.capsule.radiusMeters;
        }, Control::Number>("workspace.inspector.character.radius", "workspace.inspector.character.radius"),
            Bind<Model,
                 +[](Model &v) -> float & {
            return v.capsule.cylindricalHalfHeightMeters;
        }, Control::Number>("workspace.inspector.character.half_height", "workspace.inspector.character.half_height"),
            Bind<Model, &Model::up, Control::Vector3>("workspace.inspector.character.up", "workspace.inspector.character.up"),
            Bind<Model, &Model::gravity, Control::Vector3>("workspace.inspector.character.gravity",
                                                           "workspace.inspector.character.gravity"),
        });
    };
}  // namespace Horo::Editor::InspectorFields
