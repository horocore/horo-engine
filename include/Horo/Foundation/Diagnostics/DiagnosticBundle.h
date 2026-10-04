#pragma once

/**
 * @file DiagnosticBundle.h
 * @brief Explicit allowlist-based portable diagnostic bundle generation.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Diagnostics {
    /** @brief One explicitly allowlisted source file and its portable archive name. */
    struct DiagnosticBundleEntry {
        std::filesystem::path sourcePath;  /**< Existing regular file selected by the caller. */
        std::filesystem::path archivePath; /**< Safe relative path used inside the bundle. */
        bool optional{};                   /**< Missing optional inputs are reported but do not fail generation. */
        bool redactSensitiveText{};        /**< Parse JSON/JSONL and redact sensitive keys and absolute paths before archiving. */
    };

    /** @brief Bounded diagnostic bundle request containing no implicit data discovery. */
    struct DiagnosticBundleRequest {
        std::filesystem::path outputPath;                          /**< Absolute path for a new ZIP file. */
        std::vector<DiagnosticBundleEntry> entries;                /**< Explicit source allowlist. */
        std::vector<std::pair<std::string, std::string>> metadata; /**< Caller-vetted manifest metadata. */
        std::uintmax_t maxInputBytes{64U * 1024U * 1024U};         /**< Aggregate uncompressed input bound. */
        std::size_t maxEntries{256};                               /**< Maximum allowlisted source records. */
        std::size_t maxMetadataEntries{64};                        /**< Maximum caller-vetted metadata records. */
    };

    /** @brief Summary of a successfully committed diagnostic ZIP bundle. */
    struct DiagnosticBundleSummary {
        std::filesystem::path outputPath;   /**< Committed ZIP path. */
        std::size_t fileCount{};            /**< Number of allowlisted files, excluding the manifest. */
        std::uintmax_t inputBytes{};        /**< Aggregate uncompressed allowlisted bytes. */
        std::size_t missingOptionalCount{}; /**< Optional inputs absent at generation time. */
    };

    /**
     * @brief Creates an atomic portable ZIP containing only explicitly allowlisted files and metadata.
     * @param request Validated bundle output, source entries, metadata, and aggregate size bound.
     * @return Committed bundle summary or a stable typed observability error.
     */
    [[nodiscard]] Result<DiagnosticBundleSummary> GenerateDiagnosticBundle(const DiagnosticBundleRequest &request);
    /** @brief Returns inert error metadata for diagnostic bundle operations. @return Original Foundation-owned canonical errors. */
    [[nodiscard]] ModuleErrorDomainDescriptor DiagnosticBundleErrorDomain();
}  // namespace Horo::Diagnostics
