#pragma once

/** @file WasapiAudioBackend.h
 * @brief Inert Windows WASAPI peer composition; native COM and endpoint types remain private.
 */

#include "Horo/Audio/Internal/NativeAudioBackend.h"

namespace Horo::Audio::Backend {
    /**
     * @brief Construct the native Windows shared-mode peer without initializing COM or opening a device.
     * @param config Fixed runtime and host steady-clock domain.
     * @return Inert backend or validation/allocation failure.
     * @pre One declared control thread owns native lifecycle calls and destruction and must allow MTA initialization.
     * Inert construction/fixed identity reads do not initialize COM. The render worker joins the control MTA;
     * its IAudioRenderClient acquisition, GetBuffer/ReleaseBuffer pairs and release stay on that worker.
     */
    [[nodiscard]] Result<std::unique_ptr<NativeAudioBackend>> CreateWasapiAudioBackend(NativeAudioBackendConfig config);
}  // namespace Horo::Audio::Backend
