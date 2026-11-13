#pragma once
/** @file AudioListenerFields.h @brief Typed AudioListenerFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {

    /** @brief Static field registry for Runtime::AudioListenerComponent; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::AudioListenerComponent> {
        using Model = Runtime::AudioListenerComponent;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model, &Model::view, Control::Unsigned>("audio_listener_view", "workspace.inspector.audio.listener_view")
                .Valid(+[](const Model &v) {
            return v.view <= 16;
        }),
            Bind<Model, &Model::priority, Control::Integer>("audio_listener_priority", "workspace.inspector.audio.priority"),
            Bind<Model, &Model::weight, Control::Number>("audio_listener_weight", "workspace.inspector.audio.listener_weight")
                .Number({.speed = 0.01F})
                .Valid(+[](const Model &v) {
            return v.weight > 0;
        }),
        });
    };
}  // namespace Horo::Editor::InspectorFields
