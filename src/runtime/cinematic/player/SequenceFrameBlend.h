#pragma once

#include "Horo/Cinematic/SequenceEvaluation.h"

namespace Horo::Cinematic {
    /** @brief Validates borrowed blend storage before advancing a player. */
    [[nodiscard]] bool ValidFrameBlendScratch(const SequenceFrameScratch &scratch, std::size_t trackCount) noexcept;

    /** @brief Samples and blends all track values before any owner callback is invoked. */
    [[nodiscard]] Result<void> SampleFrameValues(std::span<const SequenceFrameTrackDescriptor> tracks, const SequencePlayerSnapshot &player,
                                                 SequenceTime position, const SequenceFrameScratch &scratch);
}  // namespace Horo::Cinematic
