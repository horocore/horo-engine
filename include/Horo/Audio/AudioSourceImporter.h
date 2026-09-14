#pragma once

/**
 * @file AudioSourceImporter.h
 * @brief Bounded WAV/Ogg source decoding and deterministic analysis contracts.
 */

#include "Horo/Audio/AudioAssetSchema.h"
#include "Horo/Audio/AudioMediaFormatRegistry.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Audio {
    /** @brief Maximum channels admitted by the built-in WAV/Ogg source importer. */
    inline constexpr std::uint32_t MaximumCoreAudioSourceChannels = 8;

    /** @brief Invocation-scoped random-access source supplied by the generic Asset Pipeline. */
    struct AudioSourceReader final {
        using ReadAtFunction = Result<std::size_t> (*)(void *context, std::uint64_t offset, std::span<std::byte> destination);

        void *context{};         /**< Borrowed context retained by the caller for the import invocation. */
        std::uint64_t size{};    /**< Exact admitted source byte count. */
        ReadAtFunction readAt{}; /**< Bounded read callback; returning zero before EOF is a failure. */
    };

    /** @brief Invocation-scoped decoded-frame sink; samples are interleaved binary32 in the supplied semantic layout. */
    struct AudioDecodedFrameSink final {
        using WriteFunction = Result<void> (*)(void *context, std::uint64_t firstFrame, const AudioProcessingFormat &format,
                                               std::span<const AudioSample> interleavedSamples);

        void *context{};       /**< Borrowed context retained by the caller for the import invocation. */
        WriteFunction write{}; /**< Synchronous callback that must copy any samples it retains. */
    };

    /** @brief Qualified resource envelope for one built-in source import. */
    struct AudioSourceImportLimits final {
        std::uint64_t maximumSourceBytes{512ULL << 20U};
        std::uint64_t maximumDecodedFrames{1ULL << 30U};
        std::uint64_t maximumDecodedBytes{2ULL << 30U};
        std::uint64_t maximumCumulativeReadBytes{2ULL << 30U};
        std::uint32_t maximumReadOperations{1'000'000};
        std::uint32_t decodeBlockFrames{4'096};
        std::uint32_t waveformWindowFrames{2'048};
        std::uint32_t maximumWaveformPoints{524'288};
        std::uint32_t maximumLoopRegions{MaximumAudioAssetLoopRegions};
        std::uint32_t maximumChannels{MaximumCoreAudioSourceChannels};
        bool operator==(const AudioSourceImportLimits &) const = default;
    };

    /** @brief Deterministic aggregate min/max envelope for one half-open source-frame window. */
    struct AudioWaveformPoint final {
        std::uint64_t firstFrame{};
        std::uint32_t frameCount{};
        AudioSample minimum{};
        AudioSample maximum{};
        bool operator==(const AudioWaveformPoint &) const = default;
    };

    /** @brief Owned semantic result of a successful built-in source extraction. */
    struct AudioSourceImportCandidate final {
        AudioContainerId container;
        AudioCodecId codec;
        std::optional<AudioPcmFormat> sourcePcm;
        AudioProcessingFormat decodedFormat;
        std::uint64_t frameCount{};
        std::uint64_t durationNanoseconds{};
        std::vector<AudioLoopRegion> loops;
        std::vector<AudioWaveformPoint> waveform;
        AudioLoudnessMetadata loudness;
        AudioSample samplePeak{};
        std::string decoderIdentity;
        bool operator==(const AudioSourceImportCandidate &) const = default;
    };

    /**
     * @brief Decodes one admitted RIFF/WAVE PCM or Ogg/Vorbis source and derives deterministic metadata.
     * @param source Borrowed random-access reader. The importer never retains it or requests the whole source contiguously.
     * @param output Borrowed synchronous sink receiving bounded interleaved binary32 frame blocks.
     * @param limits Positive resource limits no larger than the compiled Audio profile.
     * @return Owned candidate after every source frame was decoded and delivered, or a typed failure with no candidate.
     * @throws std::bad_alloc When control/worker-thread result preparation cannot allocate within admitted limits.
     */
    [[nodiscard]] Result<AudioSourceImportCandidate> ImportCoreAudioSource(
        const AudioSourceReader &source, const AudioDecodedFrameSink &output,
        const AudioSourceImportLimits &limits = AudioSourceImportLimits{});
}  // namespace Horo::Audio
