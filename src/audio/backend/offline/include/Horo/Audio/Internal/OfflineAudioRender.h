#pragma once

/** @file OfflineAudioRender.h
 * @brief Synchronous deterministic PCM export driver, separate from Null device simulation.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

#include <stop_token>

namespace Horo::Audio::Backend {
    /** @brief Hard tooling export bound; larger exports require a separately admitted streaming sink. */
    inline constexpr std::uint64_t MaximumOfflineAudioOutputBytes = 512 * 1'024 * 1'024;

    /** @brief Explicit bounded export; tail frames are supplied by the prepared mixer's tail plan. */
    struct OfflineAudioRenderRequest final {
        AudioDeviceEpoch epoch; /**< Private offline render epoch, never an active hardware epoch. */
        AudioProcessingFormat format;
        std::uint32_t blockFrames{512};
        std::uint64_t contentFrames{};
        std::uint64_t tailFrames{}; /**< Render port suppresses new source input during its declared tail interval. */
        std::uint64_t maximumOutputBytes{64 * 1'024 * 1'024};
        std::uint64_t clockDomain{};  /**< Dedicated deterministic clock domain; frame zero has timestamp zero. */
        std::stop_token cancellation; /**< Caller-owned job cancellation, checked between whole process calls. */
    };

    /** @brief Complete owned interleaved float PCM, retaining finite mixer headroom without device clipping. */
    struct OfflineAudioRenderOutput final {
        AudioProcessingFormat format;
        std::uint64_t contentFrames{};
        std::uint64_t tailFrames{};
        std::vector<AudioSample> interleaved;
    };

    /**
     * @brief Drive a prepared offline render core to a complete PCM buffer without hardware or wall-clock pacing.
     * @param request Immutable bounded format, duration and clock plan.
     * @param render Retained Horo core; handles Priming/Rendering/Quiescing and exact sample-frame automation.
     * @return Complete PCM or typed failure; no partial output escapes. Allocation occurs before process calls.
     * @pre Tooling/control caller exclusively owns a freshly prepared offline core and retains its code/context
     * through this call. The port must not reference a concurrently active device core. Its prepared tail plan
     * matches contentFrames/tailFrames; this driver neither infers silence termination nor rewrites automation.
     * @post No asynchronous work or port reference is retained. The caller tears down its offline core even
     * on failure; detachment is immediate because all process calls execute synchronously on the caller.
     */
    [[nodiscard]] Result<OfflineAudioRenderOutput> RenderOfflineAudio(const OfflineAudioRenderRequest &request, RenderPort render);
}  // namespace Horo::Audio::Backend
