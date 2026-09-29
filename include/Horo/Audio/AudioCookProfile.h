#pragma once

/**
 * @file AudioCookProfile.h
 * @brief Explicit target-specific audio cook policy and deterministic identity.
 */

#include "Horo/Audio/AudioSourceImporter.h"
#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Audio {
    /** @brief Current audio-domain cook schema; changing encoded semantics changes this version. */
    inline constexpr std::uint32_t AudioCookSchemaVersion = 2;

    /** @brief Maximum streamed chunk size admitted by both profile planning and artifact inspection. */
    inline constexpr std::uint32_t MaximumAudioStreamChunkFrames = 65'536;

    /** @brief Currently supported output compression policy. */
    enum class AudioCookCompression : std::uint8_t {
        None
    };

    /** @brief Exact little-endian interleaved IEEE binary32 quality admitted by the built-in PCM cooker. */
    enum class AudioCookQuality : std::uint8_t {
        Float32Exact
    };

    /** @brief Requested or resolved runtime residency of one cooked payload. */
    enum class AudioCookResidency : std::uint8_t {
        Auto,
        Resident,
        Streamed
    };

    /** @brief Complete effective quality settings; no ambient defaults enter a cook. */
    struct AudioCookSettings final {
        AudioContainerId container{AudioContainerIds::HoroCooked};
        AudioCodecId codec{AudioCodecIds::Pcm};
        std::optional<std::uint32_t> sampleRate;  /**< Absent means retain source rate. */
        std::optional<AudioChannelLayout> layout; /**< Absent means retain source layout. */
        AudioCookCompression compression{AudioCookCompression::None};
        AudioCookQuality quality{AudioCookQuality::Float32Exact};
        AudioCookResidency residency{AudioCookResidency::Auto};
        std::uint64_t streamThresholdFrames{480'000}; /**< Auto selects streamed at or above this frame count. */
        std::uint32_t streamChunkFrames{4'096};       /**< Bounded seekable PCM chunk size when streamed. */
        std::uint32_t encoderDelayFrames{};           /**< PCM output admits zero only. */
        std::uint32_t encoderPaddingFrames{};         /**< PCM output admits zero only. */
        bool operator==(const AudioCookSettings &) const = default;
    };

    /** @brief One complete replacement of the default policy for an exact cook target. */
    struct AudioCookTargetOverride final {
        AssetCookTargetId target;
        AudioCookSettings settings;
        bool operator==(const AudioCookTargetOverride &) const = default;
    };

    /** @brief Immutable project policy selected by the application before host cook composition. */
    struct AudioCookProfile final {
        AudioCookSettings defaults;
        std::vector<AudioCookTargetOverride> overrides;
        bool operator==(const AudioCookProfile &) const = default;
    };

    /** @brief Explicit pinned decoder/encoder/toolchain identity; not a path or ambient tool lookup. */
    struct AudioCookToolchain final {
        std::string identity;
        bool operator==(const AudioCookToolchain &) const = default;
    };

    /** @brief Resolved source-specific plan, including the provenance of effective settings. */
    struct AudioCookPlan final {
        AssetCookTargetId target;
        AudioCookSettings settings;
        AudioContainerId sourceContainer;
        AudioCodecId sourceCodec;
        std::string decoderIdentity;
        AudioProcessingFormat outputFormat;
        AudioCookResidency residency{AudioCookResidency::Resident};
        std::uint64_t frameCount{};
        bool targetOverride{};
        Sha256Digest configurationDigest; /**< Canonical profile, target and toolchain identity. */
    };

    /**
     * @brief Validates and fingerprints every byte-affecting target policy and pinned toolchain input.
     * @param profile Candidate default and target-specific settings.
     * @param target Exact target selected by the generic Asset Pipeline.
     * @param toolchain Pinned non-empty toolchain identity.
     * @return Canonical digest or a typed invalid/unsupported policy error.
     */
    [[nodiscard]] Result<Sha256Digest> FingerprintAudioCookConfiguration(const AudioCookProfile &profile, const AssetCookTargetId &target,
                                                                         const AudioCookToolchain &toolchain);

    /**
     * @brief Resolves one admitted source and target without silent codec, rate or layout fallback.
     * @param source Fully decoded source facts from AudioImport.
     * @param profile Complete authored quality policy.
     * @param target Exact generic cook target.
     * @param toolchain Pinned toolchain identity used by cache and compatibility output.
     * @return Immutable plan or a typed unsupported/invalid/budget error.
     */
    [[nodiscard]] Result<AudioCookPlan> ResolveAudioCookPlan(const AudioSourceImportCandidate &source, const AudioCookProfile &profile,
                                                             const AssetCookTargetId &target, const AudioCookToolchain &toolchain);
}  // namespace Horo::Audio
