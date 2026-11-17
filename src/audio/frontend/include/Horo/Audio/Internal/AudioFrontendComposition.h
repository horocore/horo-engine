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

    /** @brief Host-selected output intent/catalog and optional retained acquisition receipt; never selected by feature code. */
    struct AudioFrontendOutput final {
        std::unique_ptr<Backend::AudioBackend> backend;
        Backend::Open open;
        AudioDeviceSnapshot devices;
        std::uint64_t clockDomain{};
        std::optional<Backend::OperationId>
            openOperation; /**< Optional exact retained Open receipt, never a caller-fabricated candidate. */
    };

    /** @brief One non-recycled playback hold request, independent of backend operation identities. */
    struct AudioFrontendPlaybackHoldId final {
        AudioRuntimeId owner;
        std::uint64_t sequence{};
        constexpr auto operator<=>(const AudioFrontendPlaybackHoldId &) const noexcept = default;
    };

    /** @brief Copied render-core block boundary; native packet submission, callback detachment and hardware time remain separate. */
    struct AudioFrontendHeldPlayback final {
        AudioFrontendPlaybackHoldId operation;
        AudioDeviceEpoch epoch;
        AudioVoiceCursor cursor;     /**< Actual source position after the final faded block, excluding decoder look-ahead. */
        std::uint64_t sampleFrame{}; /**< One past the processed core block, not proof that retained adapter PCM was submitted or heard. */
        AudioMonotonicTimestamp blockStartedAt;
    };

    /** @brief Factory authority at the application/process composition root. */
    struct AudioFrontendComposition final {
        /** @brief Adopts detached prepared owners and stages their initial immutable publications.
         * @param resources Exclusive prepared voice/mixer/transport ownership; passed by reference and consumed on success.
         * @param output Selected backend, pinned intent/catalog and optional completed Open receipt; consumed on success.
         * @return Prepared frontend or typed invalid/admission failure. The factory never opens a device.
         * @pre All owners share scope and no producer/callback is attached. Without openOperation the
         * backend is closed and the graph matches preferred format. With openOperation the exact Open
         * completion remains retained, and the graph matches its validated actual format and period.
         * Success acknowledges that receipt before transferring ownership; failure leaves it caller-owned.
         * An adopted open output must be explicitly closed and pumped even if Start was never requested.
         * A failed publication may leave a queued initial generation in resources; caller closes
         * voice/mixer and reconciles detached work before discarding that failed candidate.
         */
        [[nodiscard]] static Result<std::unique_ptr<AudioFrontend>> Create(AudioFrontendResources &resources, AudioFrontendOutput &output);

        /**
         * @brief Close ordinary transport admission and request a completed-block playback hold without closing voice/mixer state.
         * @param frontend Active lane on its declared control owner.
         * @return Non-recycled hold identity, or rejection without callback mutation. Pending accepted transport must finish first.
         * @details The callback executes one final real voice/mixer block, fades it to zero and publishes its exact cursor.
         * Later callbacks write silence without advancing voice, stream consumption or DSP state. Control continues Pump;
         * no callback or control wait is introduced. Device loss with no further callback cannot complete this request.
         * A native adapter may still retain part of that block; migration must reconcile its PCM submission boundary separately.
         */
        [[nodiscard]] static Result<AudioFrontendPlaybackHoldId> RequestPlaybackHold(AudioFrontend &frontend);
        /**
         * @brief Copy the exact callback-completed playback boundary, without reading live voice state on control.
         * @param frontend Same retained lane and control owner.
         * @param operation Exact admitted hold identity.
         * @return Empty while pending, owned boundary after callback acknowledgement, or stale/lifecycle rejection.
         * @note A boundary never authorizes releasing or rebinding native/voice resources; actual Stop/join proof remains mandatory.
         */
        [[nodiscard]] static Result<std::optional<AudioFrontendHeldPlayback>> PollPlaybackHold(const AudioFrontend &frontend,
                                                                                               AudioFrontendPlaybackHoldId operation);
        /**
         * @brief Resume a completed hold on the same active epoch, retaining playback state and fading the first resumed block in.
         * @param frontend Same lane on its declared control owner.
         * @param operation Exact acknowledged hold identity, consumed only on success.
         * @return Success or transactional identity/pending/lifecycle rejection. Close still owns retirement of a closing lane.
         * @pre This call does not rebind an output or recover a lost device. Backend control ownership and epoch remain unchanged.
         */
        [[nodiscard]] static Result<void> ReleasePlaybackHold(AudioFrontend &frontend, AudioFrontendPlaybackHoldId operation);
    };
}  // namespace Horo::Audio::Internal
