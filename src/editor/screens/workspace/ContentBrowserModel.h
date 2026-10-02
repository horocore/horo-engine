#pragma once

/**
 * @file ContentBrowserModel.h
 * @brief Absolute-path directory projection for project asset storage.
 */

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Assets/AssetRegistry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /** @brief Kind of one item displayed in the current Content Browser directory. */
    enum class ContentBrowserEntryKind : std::uint8_t {
        Directory,
        Asset,
    };

    /** @brief Observable state of one Content Browser directory projection. */
    enum class ContentBrowserLoadState : std::uint8_t {
        Loading,
        Ready,
        Error,
    };

    /** @brief User-selectable field used to order visible Content Browser entries. */
    enum class ContentBrowserSortField : std::uint8_t {
        Name,
        Type,
    };

    /** @brief User-selectable direction used to order visible Content Browser entries. */
    enum class ContentBrowserSortDirection : std::uint8_t {
        Ascending,
        Descending,
    };

    /** @brief One absolute breadcrumb destination from the asset root to the current directory. */
    struct ContentBrowserBreadcrumb {
        std::string label;        /**< Directory name shown by the breadcrumb text link. */
        std::string absolutePath; /**< Canonical absolute directory selected by this segment. */
    };

    /** @brief One direct child of the current absolute Content Browser directory. */
    struct ContentBrowserEntry {  // NOSONAR(cpp:S1820) Aggregate directory entry representation
        ContentBrowserEntryKind kind{ContentBrowserEntryKind::Asset};

        std::string absolutePath;                                 /**< Canonical absolute path; relative navigation is never exposed. */
        std::string displayName;                                  /**< File or directory name shown on the card. */
        std::string assetId;                                      /**< Stable asset identity; empty for directories. */
        std::string assetType;                                    /**< Stable asset type identity; empty for directories. */
        std::string importerContributionId;                       /**< Preview/import contribution identity when metadata provides it. */
        std::string importerVersion;                              /**< Importer contribution version used for the committed payload. */
        std::string activeImporterVersion;                        /**< Currently registered importer contribution version. */
        std::string importerModuleId;                             /**< Owning importer module identity resolved from the active catalog. */
        std::string importerModuleVersion;                        /**< Module version used for the committed payload. */
        std::string activeImporterModuleId;                       /**< Currently registered owning module identity. */
        std::string activeImporterModuleVersion;                  /**< Currently registered owning module version. */
        std::string absoluteImportSourcePath;                     /**< Absolute source path retained for reproducible reimport. */
        std::string sourceHash;                                   /**< Canonical source hash captured by the last import. */
        std::vector<Assets::AssetImportReason> lastImportReasons; /**< Reasons recorded by the last import. */
        bool canReimport{false};                /**< True when source provenance and the exact importer remain available. */
        bool sourceChanged{false};              /**< Cheap source timestamp/size indication; reimport confirms by hash. */
        bool importerChanged{false};            /**< Active importer contribution version differs from metadata. */
        bool moduleChanged{false};              /**< Active module identity/version differs from metadata. */
        std::string absoluteMetadataPath;       /**< Existing absolute identity-sidecar path, when available. */
        std::uintmax_t byteSize{};              /**< Source payload size; zero for directories or unavailable values. */
        std::size_t containedItemCount{};       /**< Direct non-sidecar children for directory cards. */
        std::size_t dependencyCount{};          /**< Direct dependencies recorded by this asset's import metadata. */
        bool registered{false};                 /**< Whether the authoritative Asset Registry owns this asset. */
        Assets::AssetPreviewImage previewImage; /**< Optional module-produced RGBA8 card preview. */
        Assets::AssetPreviewFallback previewFallback{Assets::AssetPreviewFallback::Automatic};
    };

    /** @brief Complete immutable presentation snapshot for one absolute asset directory. */
    struct ContentBrowserDirectory {
        std::string absoluteRootPath;    /**< Canonical absolute project asset root. */
        std::string absoluteCurrentPath; /**< Canonical absolute directory currently being displayed. */
        std::vector<ContentBrowserBreadcrumb> breadcrumbs;
        std::vector<ContentBrowserEntry> entries; /**< Direct directories first, then visible asset files. */
        bool readable{false};                     /**< False when the asset root or requested directory could not be enumerated. */
        ContentBrowserLoadState loadState{ContentBrowserLoadState::Ready};
    };

    /** @brief Non-owning presentation query applied to one immutable directory snapshot. */
    struct ContentBrowserEntryQuery {
        std::string_view name;
        std::string_view assetType; /**< Empty selects every asset type; directories remain navigable. */
        ContentBrowserSortField sortField{ContentBrowserSortField::Name};
        ContentBrowserSortDirection sortDirection{ContentBrowserSortDirection::Ascending};
    };

    /**
     * @brief Builds a deterministic directory projection from disk folders, asset files, and a registry snapshot.
     * @param absoluteProjectRoot Absolute open-project root.
     * @param requestedAbsoluteDirectory Absolute directory to display, or empty to select the asset root.
     * @param snapshot Immutable authoritative asset registry snapshot.
     * @return Owned absolute-path directory snapshot. Invalid targets safely fall back to the asset root.
     */
    [[nodiscard]] ContentBrowserDirectory BuildContentBrowserDirectory(
        const std::filesystem::path &absoluteProjectRoot, const std::filesystem::path &requestedAbsoluteDirectory,
        const Assets::AssetRegistrySnapshot &snapshot, const Assets::AssetImporterCatalogSnapshot *importerCatalog = nullptr);

    /**
     * @brief Validates an absolute directory navigation target against an absolute asset root.
     * @param absoluteRoot Canonical absolute asset root.
     * @param absoluteTarget Existing absolute directory candidate.
     * @return True only for the root or a contained non-symlink directory.
     */
    [[nodiscard]] bool IsContentBrowserDirectoryTargetAllowed(const std::filesystem::path &absoluteRoot,
                                                              const std::filesystem::path &absoluteTarget);

    /**
     * @brief Projects visible entry indices without mutating the authoritative directory snapshot.
     * @param directory Immutable current-folder snapshot.
     * @param query Case-insensitive name search, exact asset-type filter, and ordering.
     * @return Indices into @p directory with directories kept before assets.
     */
    [[nodiscard]] std::vector<std::size_t> ProjectContentBrowserEntries(const ContentBrowserDirectory &directory,
                                                                        const ContentBrowserEntryQuery &query);
}  // namespace Horo::Editor
