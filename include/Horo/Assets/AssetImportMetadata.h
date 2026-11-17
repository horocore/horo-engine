#pragma once

/**
 * @file AssetImportMetadata.h
 * @brief Durable importer provenance used by reproducible asset reimport.
 */

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/TransparentString.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Horo::Assets {
    /** @brief Reason recorded for the most recent import transaction. */
    enum class AssetImportReason : std::uint8_t {
        InitialImport,
        ManualReimport,
        SourceChanged,
        ImporterChanged,
        ModuleChanged,
    };

    /** @brief Complete reproducible provenance stored in an asset identity sidecar.
     */
    struct AssetImportMetadata {
        static constexpr std::uint32_t kSchemaVersion = 1;

        AssetId assetId;
        AssetTypeId assetType;
        std::string importerContributionId;
        std::string importerVersion;
        std::string importerPackageId;
        std::string importerModuleId;
        std::string importerModuleVersion;
        std::filesystem::path absoluteSourcePath;
        std::string sourceExtension;
        std::string sourceHash;
        std::uintmax_t sourceByteSize{};
        std::int64_t sourceLastWriteTime{};
        TransparentStringMap<std::string> importSettings;
        std::vector<AssetId> dependencies;
        std::vector<AssetImportReason> lastImportReasons;
        std::string importedAtUtc;
    };

    /**
     * @brief Converts one typed import reason to its stable metadata spelling.
     * @param reason Typed import reason.
     * @return Stable lowercase metadata token.
     */
    [[nodiscard]] std::string_view AssetImportReasonName(AssetImportReason reason) noexcept;

    /**
     * @brief Reads and validates importer provenance from an identity sidecar.
     * @param absoluteMetadataPath Absolute path to a bounded `.horo` sidecar.
     * @return Parsed metadata, or a typed malformed/I/O error.
     */
    [[nodiscard]] Result<AssetImportMetadata> ReadAssetImportMetadata(const std::filesystem::path &absoluteMetadataPath);

    /** @brief Decode one already-admitted metadata generation without rereading a
     * mutable path.
     * @param contents Complete owned or borrowed metadata bytes, nonempty and at
     * most 1 MiB.
     * @return The same schema/provenance projection as ReadAssetImportMetadata, or
     * a typed malformed/limit error.
     * @details The caller retains source admission and publication leases; decoding
     * performs no filesystem access.
     */
    [[nodiscard]] Result<AssetImportMetadata> DecodeAssetImportMetadata(std::string_view contents);

    /**
     * @brief Serializes importer provenance as deterministic UTF-8 JSON with a
     * trailing newline.
     * @param metadata Complete metadata to serialize.
     * @return Serialized JSON, or a typed error when required provenance is
     * missing.
     */
    [[nodiscard]] Result<std::string> SerializeAssetImportMetadata(const AssetImportMetadata &metadata);

    /**
     * @brief Resolves serialized settings against the current importer schema.
     * @param contribution Current importer contribution and declarative settings
     * schema.
     * @param serializedSettings Settings keyed by `settings.<descriptor-id>`.
     * @return Settings in descriptor order, or a typed error for invalid retained
     * values.
     */
    [[nodiscard]] Result<std::vector<ImportSettingValue>> ResolveImportSettings(
        const AssetImporterContribution &contribution, const TransparentStringMap<std::string> &serializedSettings);

    /**
     * @brief Returns a canonical UTC timestamp suitable for durable import
     * metadata.
     * @return ISO-8601 UTC timestamp with millisecond precision.
     */
    [[nodiscard]] std::string CurrentImportTimestampUtc();

    /**
     * @brief Reads a bounded regular non-symlink source file.
     * @param absoluteSourcePath Absolute source path.
     * @return Complete source bytes, or a typed safety/read error.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadAssetImportSource(const std::filesystem::path &absoluteSourcePath);

    /**
     * @brief Computes the canonical SHA-256 identity of source bytes.
     * @param bytes Borrowed source bytes.
     * @return Canonical `sha256:` digest text.
     */
    [[nodiscard]] std::string HashAssetImportSource(std::span<const std::uint8_t> bytes);
}  // namespace Horo::Assets
