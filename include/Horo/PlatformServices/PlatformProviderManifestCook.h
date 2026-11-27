#pragma once

/**
 * @file PlatformProviderManifestCook.h
 * @brief Deterministic provider-neutral Platform Services cook and private-adapter mapping handoff.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PlatformServices/AchievementDefinitionRegistry.h"
#include "Horo/PlatformServices/PlatformDefinitionRegistries.h"
#include "Horo/PlatformServices/PlatformProjectConfiguration.h"
#include "Horo/PlatformServices/PlatformStableIdRegistry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace Horo::PlatformServices {
    inline constexpr std::uint32_t PlatformProviderManifestCookSchemaVersion = 1;

    /** @brief One opaque adapter-owned mapping with bounded authoring source for diagnostics. */
    struct PlatformCookMapping final {
        PlatformProviderMappingEvidence evidence; /**< No provider-native value crosses this boundary. */
        std::string source;                       /**< Mapping document location; never serialized into output. */
    };

    /** @brief Captured, already validated project and service generations. All references must outlive the call. */
    struct PlatformProviderManifestCookInput final {
        const PlatformStableIdRegistry &stableIds;
        const AchievementDefinitionRegistry &achievements;
        const LeaderboardDefinitionRegistry &leaderboards;
        const StatDefinitionRegistry &stats;
        const PresenceDefinitionRegistry &presence;
        const PlatformProjectConfiguration &configuration;
        PlatformProviderMappingPolicy mappingPolicy; /**< Explicit required-kind policy for selected target. */
        std::uint64_t mappingRevision{};             /**< Exact nonzero private mapping document revision. */
        std::span<const PlatformCookMapping> mappings;
        std::stop_token cancellation;       /**< Checked through final fingerprint; host fences cancellation after return. */
        CancellationToken hostCancellation; /**< Optional application cancellation chain, checked at the same cook boundaries. */
    };

    /** @brief Owned canonical outputs; host publishes both or neither after rechecking source revisions. */
    struct PlatformProviderManifestCookOutput final {
        std::vector<std::byte> neutralBytes;  /**< Horo definitions, policy and provenance; no native provider values. */
        std::vector<std::byte> mappingBytes;  /**< Exact provider, revision and ID-to-opaque-digest rows for private adapter. */
        Sha256Digest generationFingerprint{}; /**< Digest binding both outputs as one generation. */
    };

    /** @brief Typed deterministic cook failures. */
    namespace PlatformProviderManifestCookErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor StaleGeneration;
        extern const ErrorCodeDescriptor MissingMapping;
        extern const ErrorCodeDescriptor InvalidMapping;
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace PlatformProviderManifestCookErrors

    /**
     * @brief Cooks one captured project generation without opening files or loading provider code.
     * @param input Immutable validated snapshots, exact mapping revision and cancellation intent.
     * @return Two byte-stable owned artifacts or a source-located failure. Cancellation observed before the final check returns no
     * output; caller rechecks cancellation and source revisions before atomic publication.
     */
    [[nodiscard]] Result<PlatformProviderManifestCookOutput> CookPlatformProviderManifest(const PlatformProviderManifestCookInput &input);

    /** @brief Finite paired-publication byte ceiling including domain and all length fields. */
    inline constexpr std::size_t PlatformProviderManifestCookGenerationMaximumBytes = 64U * 1024U * 1024U;

    /** @brief Encodes the existing generation-fingerprint preimage as one bounded atomic publication unit.
     * @param output Complete paired output from the shared cook, with its matching generation fingerprint.
     * @return Domain-tagged length-prefixed pair, or InvalidInput/StaleGeneration; at most 64 MiB including framing.
     * @details Uses the unchanged v1 fingerprint layout. SHA-256 of these exact bytes equals generationFingerprint.
     * This framing adds no source authority, semantic validation or provider-native data.
     * @throws std::bad_alloc While preparing owned derived bytes; no source or publication state changes.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodePlatformProviderManifestCookGeneration(
        const PlatformProviderManifestCookOutput &output);

    /** @brief Recovers both streams from one exact admitted generation without accepting a mixed or partial pair.
     * @param bytes Complete bounded generation-fingerprint preimage.
     * @param expectedFingerprint Exact digest from the caller's trusted cook/publication/release authority.
     * @return Owned paired streams after exact domain, framing, byte-consumption and digest checks, or typed failure.
     * @details Does not interpret runtime semantics or grant provider trust. The caller retains those admission responsibilities.
     * @throws std::bad_alloc While copying admitted streams; no source or publication state changes.
     */
    [[nodiscard]] Result<PlatformProviderManifestCookOutput> DecodePlatformProviderManifestCookGeneration(
        std::span<const std::byte> bytes, const Sha256Digest &expectedFingerprint);
}  // namespace Horo::PlatformServices
