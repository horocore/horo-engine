#pragma once

/** @file AudioFrontendComposition.h
 * @brief Non-installed host composition of prepared Audio owners and an explicitly selected output.
 */

#include "Horo/Audio/AudioFrontend.h"
#include "Horo/Audio/AudioVoiceRenderRuntime.h"
#include "Horo/Audio/Internal/AudioBackend.h"

namespace Horo::Audio::Internal {
    /** @brief Complete detached candidate; no callback may have borrowed these owners yet. */
    struct AudioFrontendResources final {
        AudioCommandScope scope;
        AudioCommandStaging staging;
        std::unique_ptr<AudioVoiceRenderRuntime> voice;
        std::unique_ptr<MixerGraphRuntime> mixer;
        std::unique_ptr<MixerRenderPlan> plan;
        AudioVoiceRenderRequest initialVoice;
        std::shared_ptr<AudioStreamingService> streams;
        AudioStreamHandle stream;
        std::shared_ptr<const void> hostLease; /**< Retains jobs/provider/code dependencies through stream retirement. */
    };

    /** @brief Explicit output policy captured before acquisition, never selected by document/feature code. */
    struct AudioFrontendOutput final {
        std::unique_ptr<Backend::AudioBackend> backend;
        Backend::Open open;
        AudioDeviceSnapshot devices;
        std::uint64_t clockDomain{};
    };

    /** @brief Factory authority at the application/process composition root. */
    struct AudioFrontendComposition final {
        /** @brief Adopts detached prepared owners and stages their initial immutable publications.
         * @param resources Exclusive prepared voice/mixer/transport ownership; passed by reference and consumed on success.
         * @param output Selected inert backend and pinned discovery/request facts; consumed on success.
         * @return Prepared frontend or typed invalid/admission failure. No output is opened by this factory.
         * @pre Backend is closed, all owners share scope, and no producer/callback is attached.
         * A failed publication may leave a queued initial generation in resources; caller closes
         * voice/mixer and reconciles detached work before discarding that failed candidate.
         */
        [[nodiscard]] static Result<std::unique_ptr<AudioFrontend>> Create(AudioFrontendResources &resources, AudioFrontendOutput &output);
    };
}  // namespace Horo::Audio::Internal
