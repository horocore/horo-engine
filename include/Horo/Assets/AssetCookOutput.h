#pragma once

/**
 * @file AssetCookOutput.h
 * @brief Atomic cook generation publication with deterministic manifest and current.json authority.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    /** @brief Optional host transaction policy; the host serializes all writers to the target root. */
    struct AssetCookPublicationPolicy {
        DurableFileSystem *files{};                 /**< Host-owned through publication; null preserves the existing atomic writer. */
        std::function<Result<void>()> beforeCommit; /**< Final freshness/adoption check after staging, immediately before replacement. */
    };

    /**
     * @brief A published immutable cooked generation resolved from current.json.
     */
    struct AssetCookGeneration {
        AssetCookTargetId target;             /**< Target this generation was cooked for. */
        Sha256Digest manifestDigest;          /**< SHA-256 of the manifest.json bytes. */
        std::filesystem::path generationRoot; /**< Absolute path to the generation directory. */
        std::size_t artifactCount{};          /**< Number of artifacts in this generation. */
        std::optional<Error> durabilityError; /**< Current pointer committed, but subsequent durability confirmation failed. */
    };

    /**
     * @brief One artifact entry in the generation manifest.
     */
    struct AssetCookManifestEntry {
        AssetId assetId;           /**< Stable asset identity. */
        AssetTypeId assetType;     /**< Asset type. */
        std::string artifactFile;  /**< Relative filename: <AssetId>.cooked */
        Sha256Digest artifactHash; /**< SHA-256 of the artifact envelope bytes. */
    };

    /** @brief Exact cooked generation inventory and verified encoded artifacts. */
    struct AssetCookGenerationContents {
        std::vector<AssetCookManifestEntry> entries;
        std::vector<std::vector<std::uint8_t>> artifacts;
    };

    /**
     * @brief Resolves the current active generation from a target root's current.json.
     * @param targetRoot Root directory for this target's cooked output (e.g., build/cooked/headless-null).
     * @param limits Size bounds for validation.
     * @return The active generation, or a typed error if current.json is missing/malformed.
     */
    [[nodiscard]] Result<AssetCookGeneration> ResolveCurrentCookGeneration(const std::filesystem::path &targetRoot,
                                                                           const AssetCookLimits &limits = {});

    /**
     * @brief Reads one pinned generation without consulting the mutable current.json pointer.
     * @param generation Exact target, manifest digest, generation root, and count previously frozen by the caller.
     * @param maximumTotalBytes Aggregate allocation ceiling for all encoded artifacts.
     * @param limits Per-artifact and count ceilings.
     * @return Canonical manifest entries and matching verified cooked envelopes, or a typed failure.
     */
    [[nodiscard]] Result<AssetCookGenerationContents> ReadCookGenerationContents(const AssetCookGeneration &generation,
                                                                                 std::size_t maximumTotalBytes,
                                                                                 const AssetCookLimits &limits = {});

    /**
     * @brief Publishes a complete generation atomically.
     * @details Stages the generation directory under <targetRoot>/generations/<manifest-digest>/,
     *          writes deterministically sorted manifest.json, writes each artifact as <assetId>.cooked,
     *          and atomically replaces current.json last.
     *
     * @param targetRoot Root directory for this target's cooked output.
     * @param target Target ID this generation is for.
     * @param entries Sorted manifest entries (by canonical AssetId). Must be non-empty and unique.
     * @param artifactPayloads Full encoded artifact envelope bytes, in the same order as entries.
     * @param limits Size bounds for validation.
     * @param policy Optional durable writer and final adoption check. Check failure preserves current.json.
     * @return The published generation, or a typed error.
     */
    [[nodiscard]] Result<AssetCookGeneration> PublishCookGeneration(const std::filesystem::path &targetRoot,
                                                                    const AssetCookTargetId &target,
                                                                    std::span<const AssetCookManifestEntry> entries,
                                                                    std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                                                    const AssetCookLimits &limits = {},
                                                                    const AssetCookPublicationPolicy &policy = {});

}  // namespace Horo::Assets
