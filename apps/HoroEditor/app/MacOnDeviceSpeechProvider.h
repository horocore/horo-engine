#pragma once

/** @file MacOnDeviceSpeechProvider.h @brief macOS adapter for explicitly admitted on-device recognition. */

#include "ImmersiveSpeechTranscription.h"

namespace Horo::Editor {
    enum class MacSpeechAuthorization : std::uint8_t {
        Unavailable,
        Required,
        Pending,
        Denied,
        Granted,
    };

    /** @brief Observes speech authorization without prompting or touching the microphone. */
    [[nodiscard]] MacSpeechAuthorization ObserveMacSpeechAuthorization() noexcept;

    /** @brief Requests speech authorization after an explicit main-thread Editor action.
     * @return Current or pending authorization. This does not open a microphone or a recognition stream.
     */
    [[nodiscard]] MacSpeechAuthorization RequestMacSpeechAuthorization() noexcept;

    /** @brief Recognizes already-admitted PCM using the local macOS Speech framework. */
    class MacOnDeviceSpeechProvider final : public ISpeechProvider {
    public:
        /** @copydoc ISpeechProvider::Open */
        [[nodiscard]] std::unique_ptr<ISpeechProviderStream> Open(std::string_view locale, const Audio::AudioProcessingFormat &format,
                                                                  std::stop_token stop) noexcept override;
    };
}  // namespace Horo::Editor
