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
    struct AssetCookGeneration;

    /** @brief Optional host transaction policy; every writer holds .cook-writer.lock until publication and adoption return. */
    struct AssetCookPublicationPolicy {
        DurableFileSystem *files{};                 /**< Required host-owned durable filesystem through publication. */
        std::function<Result<void>()> beforeCommit; /**< Final freshness/adoption check after staging, immediately before replacement. */
        std::function<Result<void>()> waitingForWriter;    /**< Busy-lock retry admission; performs bounded cancellation-aware backoff. */
        std::function<Result<void>()> afterWriterAcquired; /**< Freshness/cancellation checkpoint before reading the locked current base. */
        std::string
            operationId; /**< Optional host-generated canonical attempt UUID; private durable writers require this or newOperationId. */
        std::function<Result<void>(const AssetCookGeneration &)>
            prepareCommit; /**< Preallocates host receipt/adoption metadata before commit. */
        std::function<void(const AssetCookGeneration &)>
            afterCommit;                                 /**< Nonthrowing live adoption while the native writer lease is retained. */
        std::function<Result<AssetId>()> newOperationId; /**< Host entropy source for unguessable private attempt identity. */
        const ExclusiveFileLock *writerLease{};          /**< Required native lease for this exact target root's .cook-writer.lock. */
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
     * @return The active generation, asset.cook.not_published for the verified unpublished bootstrap state,
     *         or a typed error if current.json is missing/malformed.
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
     * @brief Validates the sole current authority and removes bounded, abandoned AssetCook private staging.
     * @param targetRoot Canonical absolute host-selected root, protected by the caller's .cook-writer.lock.
     * @param target Exact expected target.
     * @param maximumTotalBytes Aggregate ceiling for verifying the active generation.
     * @param limits Per-file, inventory and recovery-work bounds.
     * @param policy Required durable filesystem; callbacks are not invoked by recovery.
     * @return Verified current generation, empty for a verified unpublished bootstrap selector, or a typed error.
     * @pre Every cooperating writer creates and accesses .cook-staging only while holding the same native writer lock.
     * @details Recovery prevalidates every exact private file before removal. It never follows links, recursively deletes,
     *          selects an orphan, removes an immutable generation or repairs a malformed/missing established pointer.
     *          A virgin root first receives a durable schemaVersion 2 unpublished current.json; inactive first-attempt
     *          generations may be recooked only while that exact target's unpublished selector remains valid.
     */
    [[nodiscard]] Result<std::optional<AssetCookGeneration>> RecoverCookPublication(const std::filesystem::path &targetRoot,
                                                                                    const AssetCookTargetId &target,
                                                                                    std::size_t maximumTotalBytes,
                                                                                    const AssetCookLimits &limits,
                                                                                    const AssetCookPublicationPolicy &policy);

    /**
     * @brief Publishes a complete generation atomically.
     * @details Writes complete private .cook-staging/<operation-id> files, verifies existing digest-named
     *          generations or promotes the complete immutable directory, then atomically replaces current.json last.
     *          Existing immutable generations are never rewritten or removed. A value with durabilityError is committed.
     *          First publication durably initializes the same selector's unpublished state before immutable promotion.
     * @pre The caller serializes writers using .cook-writer.lock through pointer replacement and live adoption.
     *
     * @param targetRoot Root directory for this target's cooked output.
     * @param target Target ID this generation is for.
     * @param entries Sorted unique manifest entries (by canonical AssetId); empty publishes a complete empty inventory.
     * @param artifactPayloads Full encoded artifact envelope bytes, in the same order as entries.
     * @param limits Size bounds for validation.
     * @param policy Required durable writer, exact-root native lease and attempt identity; optional final adoption check.
     * @return The published generation, or a typed error.
     */
    [[nodiscard]] Result<AssetCookGeneration> PublishCookGeneration(const std::filesystem::path &targetRoot,
                                                                    const AssetCookTargetId &target,
                                                                    std::span<const AssetCookManifestEntry> entries,
                                                                    std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                                                    const AssetCookLimits &limits = {},
                                                                    const AssetCookPublicationPolicy &policy = {});

}  // namespace Horo::Assets
