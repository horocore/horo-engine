#pragma once

/**
 * @file AudioCooker.h
 * @brief Deterministic Audio-owned PCM cook contribution and compatibility manifest.
 */

#include "Horo/Assets/AssetProvider.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Audio/AudioCookProfile.h"
#include "Horo/Audio/AudioStreamingService.h"
#include "Horo/Foundation/CancellationToken.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Audio {
    /** @brief Bounded accepted cooked PCM payload size before the generic AST artifact envelope. */
    inline constexpr std::size_t MaximumCookedAudioPayloadBytes = 128U << 20U;

    /** @brief Validated domain compatibility facts, independent of native file paths or callback state. */
    struct AudioCookManifest final {
        AssetCookTargetId target;
        AudioContainerId sourceContainer;
        AudioCodecId sourceCodec;
        AudioContainerId container;
        AudioCodecId codec;
        AudioCookCompression compression{AudioCookCompression::None};
        AudioCookQuality quality{AudioCookQuality::Float32Exact};
        AudioProcessingFormat format;
        AudioCookResidency residency{AudioCookResidency::Resident};
        bool targetOverride{};
        std::uint64_t frameCount{};
        std::uint64_t streamThresholdFrames{};
        std::uint32_t chunkFrames{};
        std::uint32_t chunkCount{};
        std::uint32_t encoderDelayFrames{};
        std::uint32_t encoderPaddingFrames{};
        std::uint64_t payloadByteCount{};
        Sha256Digest sourceDigest;
        Sha256Digest decoderIdentityDigest;
        Sha256Digest configurationDigest;
        Sha256Digest toolchainDigest;
        Sha256Digest payloadDigest;
        AudioAnalysisMetadata analysis; /**< Cook-published analysis; inspection never re-decodes the source. */
        bool operator==(const AudioCookManifest &) const = default;
    };

    /** @brief Complete logical audio output; AST owns staging, cache and atomic publication. */
    struct AudioCookedOutput final {
        AudioCookManifest manifest;
        std::vector<std::uint8_t> bytes;
    };

    /**
     * @brief Decodes admitted source bytes and writes a deterministic seekable PCM payload and compatibility header.
     * @param source Immutable invocation-scoped source bytes supplied by AST.
     * @param profile Explicit project quality policy.
     * @param target Exact target selected by AST.
     * @param toolchain Pinned decoder/encoder identity.
     * @param cancellation Host-owned cooperative cancellation token.
     * @return Logical cooked output or a typed source, policy, budget or cancellation failure.
     */
    [[nodiscard]] Result<AudioCookedOutput> CookAudioSource(std::span<const std::uint8_t> source, const AudioCookProfile &profile,
                                                            const AssetCookTargetId &target, const AudioCookToolchain &toolchain,
                                                            const CancellationToken &cancellation = {});

    /**
     * @brief Validates an untrusted cooked audio payload before runtime/media use or cache reuse.
     * @param bytes Complete domain payload, excluding the generic AST artifact envelope.
     * @return Verified immutable compatibility manifest or a typed invalid-payload error.
     */
    [[nodiscard]] Result<AudioCookManifest> InspectCookedAudio(std::span<const std::uint8_t> bytes);

    /**
     * @brief Binds a pinned cooked provider to the worker-side PCM streaming opener.
     * @param provider Owned immutable package or cooked-filesystem generation, never an authoring source.
     * @param target Exact runtime cook target.
     * @param type Expected registered Audio asset type.
     * @param maximumArtifactBytes Provider's configured per-load allocation ceiling.
     * @return Owned source binding or a typed invalid-limit error. The source lease pins the provider.
     * @pre The provider enforces maximumArtifactBytes before allocating a load. Configure filesystem/archive
     * provider limits accordingly; mutable providers must not replace this captured generation.
     * @details Worker opening verifies AST identity, type, target, integrity and Audio's cooked schema.
     * Requests reserve at least three times the artifact ceiling for load/envelope/inspection peak storage.
     * PCM decode reads only the retained verified cooked payload; no source codec or file fallback is used.
     */
    [[nodiscard]] Result<AudioStreamPackageSource> MakeCookedAudioStreamSource(std::shared_ptr<const Assets::IAssetProvider> provider,
                                                                               AssetCookTargetId target, Assets::AssetTypeId type,
                                                                               std::size_t maximumArtifactBytes);

    /**
     * @brief Creates an inert exact-target Audio cooker contribution for host catalog registration.
     * @param type Registered AST asset type for the source media family.
     * @param profile Immutable cook profile copied by the strategy.
     * @param target Exact target claimed by this contribution.
     * @param toolchain Pinned toolchain identity copied by the strategy.
     * @return Contribution or a typed policy/identity error. Does not register ambient state.
     */
    [[nodiscard]] Result<Assets::CookerContribution> MakeAudioCookerContribution(const Assets::AssetTypeId &type, AudioCookProfile profile,
                                                                                 AssetCookTargetId target, AudioCookToolchain toolchain);
}  // namespace Horo::Audio
