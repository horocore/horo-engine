#pragma once

#include "Horo/Audio/AudioFormat.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <optional>
#include <stop_token>
#include <vector>

namespace Horo::Application::Internal {
    /** @brief Control-owned file shape and exact speaker permutation for extensible float WAVE. */
    struct WaveShape final {
        std::array<std::pair<std::uint32_t, std::size_t>, Audio::MaximumAudioChannels> channels{};
        std::uint32_t count{};
        std::uint32_t mask{};
        std::uint64_t frames{};
        std::uint64_t encodedBytes{};
    };

    /** @brief Existing cancellation scopes supported by both hosts of the shared encoder. */
    struct WaveCancellation final {
        std::stop_token offline;
        CancellationToken recording;

        [[nodiscard]] bool IsRequested() const noexcept {
            return offline.stop_requested() || recording.IsCancellationRequested();
        }
    };

    /** @brief Checks bounded output size and maps semantic speaker order to WAVE channel order. */
    [[nodiscard]] std::optional<WaveShape> PrepareWaveShape(const Audio::AudioProcessingFormat &format, std::uint64_t frames,
                                                            std::uint64_t maximumEncodedBytes) noexcept;

    /** @brief Encodes an admitted finite PCM span with no I/O and periodic cancellation checks. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeWave(const WaveShape &shape, std::uint32_t sampleRate,
                                                            std::span<const Audio::AudioSample> samples,
                                                            const WaveCancellation &cancellation);
}  // namespace Horo::Application::Internal
