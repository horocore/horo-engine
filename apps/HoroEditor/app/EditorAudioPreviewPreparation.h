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

    /** @brief Detached monitor playback and the exact bus identity needed by capture admission. */
    struct PreparedMonitor final {
        Audio::Internal::AudioFrontendResources resources;
        Audio::AudioBusHandle bus;
    };

    /** @brief Resolves the exact registered clip and decodes supported source PCM outside drawing/callbacks. */
    [[nodiscard]] Result<DecodedSource> Decode(const AudioComponentPreviewRequest &request);
    /** @brief Creates detached real voice, mixer and FIFO owners in one runtime scope. */
    [[nodiscard]] Result<Audio::Internal::AudioFrontendResources> Prepare(const CapturedSource &capture, Audio::AudioRuntimeId owner);
    /** @brief Composes one prepared live source on its own local monitor bus before any output is opened.
     * @param service Exact host-owned live stream service. @param stream Dedicated empty live PCM stream.
     * @param owner Runtime that will own the pinned output and its voice. @param hostLease Retains the service's host dependencies.
     * @return Detached composition and its bus identity, or a typed failure. Caller retains the stream on failure.
     */
    [[nodiscard]] Result<PreparedMonitor> PrepareMonitor(const std::shared_ptr<Audio::AudioStreamingService> &service,
                                                          Audio::AudioStreamHandle stream, Audio::AudioRuntimeId owner,
                                                          std::shared_ptr<const void> hostLease);
    /** @brief Releases rejected factory candidates only with the host's never-attached callback proof. */
    void RetireDetached(Audio::Internal::AudioFrontendResources &resources) noexcept;
}  // namespace Horo::Editor::AudioPreviewHostDetail
