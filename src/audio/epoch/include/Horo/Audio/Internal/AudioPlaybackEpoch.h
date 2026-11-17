#pragma once

/** @file AudioPlaybackEpoch.h @brief Non-installed authority for detached same-format playback epoch adoption. */

#include <cstdint>

namespace Horo {
    template <typename T> class Result;
}

namespace Horo::Audio {
    struct AudioCommandScope;
    struct MixerPlanIdentity;
    class MixerGraphRuntime;
    class AudioVoiceRenderRuntime;
    class AudioCommandStaging;
    struct AudioCommandStagingDescriptor;

    namespace Internal {
        /** @brief Fixed control-side rejection; no partial processing-state mutation on rejection. */
        enum class AudioPlaybackEpochStatus : std::uint8_t {
            Rebound,
            InvalidScope,
            PendingPublication,
            Unprepared,
            UnsupportedAutomation
        };

        /**
         * @brief Narrow host authority retaining actual processing state while adopting a strictly newer callback epoch.
         *
         * All calls run on the sole control owner AFTER it proves no old processing reader remains or can enter,
         * using actual native Stop/join if a native callback was attached, and reconciles accepted transport and
         * completed-block publications. Synchronous tooling owns and finishes its entire processing lane directly.
         * A playback hold, silence, cancelled request or timeout is not that proof. This authority creates no
         * native or voice resources and does not change rate/layout/capacity. Hosts retire the old command transport
         * and prepare a new epoch-bound transport; no old record may cross the reset boundary. The new callback
         * is not attached until mixer and voice adoption both succeed; partial host preparation still requires
         * explicit detached cleanup. Output clock correlation/discontinuity is a separate host obligation.
         */
        struct AudioPlaybackEpoch final {
            /**
             * @brief Prepare a new epoch-bound command owner after actual old transport retirement, retaining the active context.
             * @param transport Old closed-and-drained owner, unchanged by both success and failure.
             * @param previous Exact old runtime, callback epoch and still-open logical context.
             * @param replacement Same runtime/context and strictly newer callback epoch.
             * @param descriptor Explicit new reservations; callback and ingress storage identities differ from both old identities.
             * @return Separate empty open staging with the exact replacement context registered, or typed admission/allocation failure.
             * @pre The host has stopped/joined producers, proven callback detachment and reconciled every accepted old command.
             * Queue drain alone proves neither callback execution nor native detachment. No old records, gates or sequences transfer.
             * Allocation occurs only on the exclusive control owner; no callback or producer may enter during preparation.
             */
            [[nodiscard]] static Result<AudioCommandStaging> PrepareTransport(const AudioCommandStaging &transport,
                                                                              const AudioCommandScope &previous,
                                                                              const AudioCommandScope &replacement,
                                                                              const AudioCommandStagingDescriptor &descriptor);
            /**
             * @brief Adopt the active mixer/DSP state without rebuilding or resetting its signal history.
             * @param mixer Detached, open same-format owner with all publications reconciled.
             * @param previous Exact current scope.
             * @param replacement Same runtime/context and strictly newer epoch.
             * @param graph Receives the active graph's new identity only on success.
             * @return Fixed status; automation-bound plans require explicit new preparation and are rejected unchanged.
             */
            [[nodiscard]] static AudioPlaybackEpochStatus RebindMixer(MixerGraphRuntime &mixer, const AudioCommandScope &previous,
                                                                      const AudioCommandScope &replacement,
                                                                      MixerPlanIdentity &graph) noexcept;
            /**
             * @brief Adopt the retained voice cursor, converter, stream port, spatial state and parameters into the new epoch.
             * @param voice Detached, open owner with an active reconciled state publication.
             * @param previous Exact old scope.
             * @param replacement Same runtime/context and strictly newer epoch.
             * @param previousGraph Exact retained voice route identity.
             * @param replacementGraph Same graph/revisions/generation with the new epoch, admitted by RebindMixer.
             * @return Fixed status; rejection preserves every voice field. Success resets only epoch-local command receipts.
             */
            [[nodiscard]] static AudioPlaybackEpochStatus RebindVoice(AudioVoiceRenderRuntime &voice, const AudioCommandScope &previous,
                                                                      const AudioCommandScope &replacement,
                                                                      const MixerPlanIdentity &previousGraph,
                                                                      const MixerPlanIdentity &replacementGraph) noexcept;
        };
    }  // namespace Internal
}  // namespace Horo::Audio
