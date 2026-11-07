#pragma once
/** @file AudioListener.h @brief Static AudioListener Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddAudioListener(const AddComponentContext &context) noexcept {
        return !context.object.components.audioListener;
    }

    /** @brief Inert label/category/action mapping for AudioListener. */
    inline constexpr BuiltinAddComponentDefinition AudioListener{"component.audio_listener",  "workspace.inspector.kind.audio_listener",
                                                                 AddComponentCategory::Audio, ComponentType::AudioListener,
                                                                 CanAddAudioListener,         false};
}  // namespace Horo::Editor::AddComponentDefinitions
