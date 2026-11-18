#pragma once

/** @file OfflineMixerSession.h @brief Bind exclusively prepared Audio owners to the offline render port. */

#include "Horo/Audio/AudioVoiceRenderRuntime.h"
#include "Horo/Audio/Internal/OfflineAudioRender.h"

namespace Horo::Audio::Backend {
    /** @brief Prepared Audio content admitted to one fresh offline runtime. */
    struct OfflineMixerSource final {
        AudioCommandScope scope;
        AudioSampleClock initialClock; /**< Exact generation and discontinuity at the zero-frame export origin. */
        MixerGraphRuntime &mixer;
        const AudioCommandRecord &initialGraph;           /**< Initial graph swap consumed through ordinary staging. */
        std::span<AudioVoiceRenderRuntime *const> voices; /**< Exclusively prepared, started voices in ascending slot order. */
        AudioParameterAutomation *automation{};           /**< Optional sealed automation bound to the initial mixer plan and clock. */
    };

    /** @brief Synchronous, single-use RenderPort adapter with preallocated mixer input scratch. */
    class OfflineMixerSession final {
    public:
        OfflineMixerSession(const OfflineMixerSession &) = delete;
        OfflineMixerSession &operator=(const OfflineMixerSession &) = delete;
        OfflineMixerSession(OfflineMixerSession &&) noexcept = default;
        OfflineMixerSession &operator=(OfflineMixerSession &&) = delete;

        /**
         * @brief Validate and retain the offline-only borrow before entering the mixer.
         * @param request Exact origin, format and content/tail bounds used by the driver.
         * @param source Freshly prepared owners and immutable initial publication.
         * @return Ready adapter or typed argument/allocation failure.
         * @pre Owners belong exclusively to this export, are configured for the requested format/block,
         *      and the caller retains them through all calls. Voice frames must be synchronously available.
         *      The host supplies a declared bounded tail for the prepared graph. The caller completes
         *      ordinary mixer/voice retirement after the synchronous driver returns.
         */
        [[nodiscard]] static Result<OfflineMixerSession> Prepare(const OfflineAudioRenderRequest &request,
                                                                 const OfflineMixerSource &source);

        /** @brief Borrow the adapter for one matching synchronous driver call. @return Port valid while this adapter stays in place. */
        [[nodiscard]] RenderPort Port() & noexcept;

    private:
        OfflineMixerSession(const OfflineAudioRenderRequest &request, const OfflineMixerSource &source);
        [[nodiscard]] static RenderResult Process(void *context, const RenderInvocation &invocation) noexcept;
        [[nodiscard]] RenderResult RenderBlock(const RenderInvocation &invocation) noexcept;

        OfflineMixerSource source_;
        std::vector<MixerVoiceInput> inputs_;
        AudioDeviceEpoch epoch_;
        std::uint64_t contentFrames_;
        const AudioCommandRecord *pendingGraph_;
    };

    /** @brief Render prepared Audio owners to complete bounded float PCM. @param request Exact export plan.
     * @param source Exclusive mixer and prepared voices; see OfflineMixerSession::Prepare.
     * @return Complete PCM with declared tail, or typed failure. */
    [[nodiscard]] Result<OfflineAudioRenderOutput> RenderOfflineAudio(const OfflineAudioRenderRequest &request,
                                                                      const OfflineMixerSource &source);
}  // namespace Horo::Audio::Backend
