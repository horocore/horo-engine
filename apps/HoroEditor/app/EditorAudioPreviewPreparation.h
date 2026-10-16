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
    /** @brief Creates detached real voice, mixer and FIFO owners in one runtime scope. */
    [[nodiscard]] Result<Audio::Internal::AudioFrontendResources> Prepare(const AudioComponentPreviewRequest &request,
                                                                          const DecodedSource &decoded, Audio::AudioRuntimeId owner);
    /** @brief Releases rejected factory candidates only with the host's never-attached callback proof. */
    void RetireDetached(Audio::Internal::AudioFrontendResources &resources) noexcept;
}  // namespace Horo::Editor::AudioPreviewHostDetail
