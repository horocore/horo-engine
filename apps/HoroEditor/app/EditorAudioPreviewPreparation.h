#pragma once
#include "Horo/Audio/Internal/AudioFrontendComposition.h"
#include "Horo/Editor/AudioComponentPreview.h"

namespace Horo::Editor::AudioPreviewHostDetail {
    /** @brief Bounded decoded source retained off-callback by the exact preview capture. */
    struct DecodedSource final {
        Audio::AudioProcessingFormat format;
        std::vector<std::vector<float>> planes;
        std::uint32_t frames{};
    };

    /** @brief Immutable playback preparation inputs that can be reused after retiring the old Audio lane. */
    struct CapturedSource final {
        Audio::AudioSpatialSource source;
        std::optional<Audio::AudioSpatialListener> listener;
        DecodedSource decoded;
    };

    /** @brief Resolves the exact registered clip and decodes supported source PCM outside drawing/callbacks. */
    [[nodiscard]] Result<DecodedSource> Decode(const AudioComponentPreviewRequest &request);
    /** @brief Creates detached real voice, mixer and FIFO owners in one runtime scope. */
    [[nodiscard]] Result<Audio::Internal::AudioFrontendResources> Prepare(const CapturedSource &capture, Audio::AudioRuntimeId owner);
    /** @brief Releases rejected factory candidates only with the host's never-attached callback proof. */
    void RetireDetached(Audio::Internal::AudioFrontendResources &resources) noexcept;
}  // namespace Horo::Editor::AudioPreviewHostDetail
