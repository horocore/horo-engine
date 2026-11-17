#pragma once

/** @file CoreAudioBackend.h
 * @brief Native macOS HAL/AudioUnit peer factory without exposed Apple types.
 */

#include "Horo/Audio/Internal/NativeAudioBackend.h"

namespace Horo::Audio::Backend {
    /**
     * @brief Construct an inert HAL output peer for one declared control thread.
     * @param config Fixed runtime and steady-clock domain.
     * @return Backend or typed validation/allocation failure.
     * @pre Control retains the backend through AudioUnit stop and property-listener queue drainage.
     */
    [[nodiscard]] Result<std::unique_ptr<NativeAudioBackend>> CreateCoreAudioBackend(NativeAudioBackendConfig config);
}  // namespace Horo::Audio::Backend
