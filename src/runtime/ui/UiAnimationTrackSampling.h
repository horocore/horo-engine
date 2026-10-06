#pragma once

/** @file UiAnimationTrackSampling.h
 * @brief Owner-private admission and deterministic sampling of immutable typed property tracks.
 */
#include "Horo/Runtime/Ui/UiAnimationTracks.h"
#include "Horo/Runtime/Ui/UiElementTree.h"

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Binds authored target/property to actual tree and registry; no source parsing or target mutation occurs. */
    [[nodiscard]] Result<UiElementHandle> ValidatePropertyTrack(const UiAnimationPropertyTrack &track, const UiElementTree &tree,
                                                                const RuntimeStyleRegistry &registry, std::uint32_t maximumKeyframes);
    /** @brief Samples an admitted immutable track with exact endpoints and integer segment selection. */
    [[nodiscard]] Result<UiStyleValue> SamplePropertyTrack(const UiAnimationPropertyTrack &track, std::uint32_t progress);
}  // namespace Horo::Runtime::Ui::AnimationInternal
