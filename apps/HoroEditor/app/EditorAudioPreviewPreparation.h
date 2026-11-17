#pragma once
#include "Horo/Audio/Internal/AudioFrontendComposition.h"
#include "Horo/Editor/AudioComponentPreview.h"

namespace Horo::Editor::AudioPreviewHostDetail {
    /** @brief Bounded decoded source owned only during detached voice preparation. */
    struct DecodedSource final {
        Audio::AudioProcessingFormat format;
        std::vector<std::vector<float>> planes;
        std::uint32_t frames{};
    };

    /** @brief Resolves the exact registered clip and decodes supported source PCM outside drawing/callbacks. */
    [[nodiscard]] Result<DecodedSource> Decode(const AudioComponentPreviewRequest &request);
    /** @brief Creates detached voice, mixer and FIFO owners for the admitted device format.
     * @param request Copied preview intent and bus/source values.
     * @param decoded Retained decoded source; its original rate remains distinct from the device rate.
     * @param owner Fixed lane runtime identity.
     * @param processing Validated actual stereo format, never the requested preference substituted for it.
     * @param maximumFrames Admitted callback block bound used for every converter, voice and graph reservation.
     * @return Complete detached owners or a typed unsupported/preparation failure, before any callback attaches.
     */
    [[nodiscard]] Result<Audio::Internal::AudioFrontendResources> Prepare(const AudioComponentPreviewRequest &request,
                                                                          const DecodedSource &decoded, Audio::AudioRuntimeId owner,
                                                                          const Audio::AudioProcessingFormat &processing,
                                                                          std::uint32_t maximumFrames);
    /** @brief Releases rejected factory candidates only with the host's never-attached callback proof. */
    void RetireDetached(Audio::Internal::AudioFrontendResources &resources) noexcept;
}  // namespace Horo::Editor::AudioPreviewHostDetail
