#pragma once

#include "Horo/Audio/AudioSourceImporter.h"

#include <cstdint>
#include <ebur128.h>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Audio::AudioImportDetail {
    struct AnalysisAccumulator final {
        struct MeterDeleter final {
            void operator()(ebur128_state *state) const noexcept;
        };

        std::vector<AudioWaveformPoint> waveform;
        std::unique_ptr<ebur128_state, MeterDeleter> meter;
        long double squaredSum{};
        std::uint64_t sampleCount{};
        AudioSample peak{};
        AudioSample windowMinimum{std::numeric_limits<AudioSample>::max()};
        AudioSample windowMaximum{std::numeric_limits<AudioSample>::lowest()};
        std::uint32_t windowFrames{};
        std::uint64_t windowStart{};

        [[nodiscard]] Result<void> Configure(const AudioProcessingFormat &format);
        [[nodiscard]] Result<void> Consume(std::span<const AudioSample> samples, std::uint32_t channels, std::uint32_t maximumWindowFrames);
        void FlushWindow();
    };

    [[nodiscard]] Result<AudioAnalysisMetadata> FinishAnalysis(AnalysisAccumulator &accumulator, std::uint32_t sampleRate,
                                                               std::uint64_t frames, std::uint64_t blockFrames,
                                                               std::uint32_t waveformWindowFrames, std::size_t channels);
}  // namespace Horo::Audio::AudioImportDetail
