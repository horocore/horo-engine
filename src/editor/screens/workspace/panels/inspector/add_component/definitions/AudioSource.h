#pragma once
/** @file AudioSource.h @brief Static AudioSource Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddAudioSource(const AddComponentContext &context) noexcept {
        return !context.object.components.audioSource;
    }

    /** @brief Inert label/category/action mapping for AudioSource. */
    inline constexpr BuiltinAddComponentDefinition AudioSource{"component.audio_source",    "workspace.inspector.kind.audio_source",
                                                               AddComponentCategory::Audio, ComponentType::AudioSource,
                                                               CanAddAudioSource,           false};
}  // namespace Horo::Editor::AddComponentDefinitions
