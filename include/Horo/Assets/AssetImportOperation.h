#pragma once

/**
 * @file AssetImportOperation.h
 * @brief Headless transactional import operation and host-owned commit boundary.
 */

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/TransparentString.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Horo::Editor {
    class ProjectMutationCoordinator;
    class ProjectMutationLease;
}  // namespace Horo::Editor

namespace Horo::Assets {

    // ---------------------------------------------------------------------------
    // Operation phases and snapshot model
    // ---------------------------------------------------------------------------

    /** @brief Lifecycle phase of one import operation. */
    enum class AssetImportPhase : std::uint8_t {
        Selecting,     /**< User is choosing files and importers. */
        Preparing,     /**< Importers are producing editor payloads. */
        ReadyToCommit, /**< All parsing complete, awaiting host commit. */
        Committing,    /**< Host is writing sidecars and publishing registry. */
        Completed,     /**< Operation succeeded. */
        Failed,        /**< Operation failed with diagnostics. */
        Cancelled,     /**< Operation was cancelled. */
    };

    /** @brief One item in an import operation. */
    struct AssetImportItem {                          // NOSONAR(cpp:S1820) Aggregate transfer struct for import dialog state
        ProjectPath sourceFile;                       /**< Project-relative source path (display only). */
        std::filesystem::path absoluteSourcePath;     /**< Absolute path to source file for reading. */
        std::string importerContributionId;           /**< Selected importer contribution. */
        std::string importerVersion;                  /**< Version of the selected importer contribution. */
        std::string importerPackageId;                /**< Package that owns the selected importer. */
        std::string importerModuleId;                 /**< Module that owns the selected importer. */
        std::string importerModuleVersion;            /**< Version of the owning module. */
        std::optional<AssetTypeId> resolvedType;      /**< Resolved asset type after import. */
        std::optional<PreparedAssetImport> result;    /**< Import result when completed. */
        std::vector<ImportDiagnostic> diagnostics;    /**< Per-item diagnostics. */
        std::string sourceExtension;                  /**< Lowercase file extension without dot. */
        std::string displayName;                      /**< File name for display. */
        std::string destinationFolder;                /**< Project-relative destination folder. */
        std::string targetExtension{".horoasset"};    /**< Target file extension this importer produces (including dot). */
        bool supportsMetaSidecar{true};               /**< True if this importer supports creating a meta sidecar. */
        TransparentStringMap<std::string> settings;   /**< Per-item importer settings (key=settingId, value=serialized). */
        std::string sourceHash;                       /**< Canonical hash captured from the imported source bytes. */
        std::uintmax_t sourceByteSize{};              /**< Imported source byte count. */
        std::int64_t sourceLastWriteTime{};           /**< Native source write-time tick captured at import. */
        std::optional<AssetId> preservedAssetId;      /**< Existing identity retained by a reimport transaction. */
        std::vector<AssetImportReason> importReasons; /**< Durable reasons for this import transaction. */
        std::uint64_t progressCompletedUnits{};       /**< Last provider-reported completed work units. */
        std::uint64_t progressTotalUnits{};           /**< Last provider-reported non-zero total work units. */
        std::string progressMessage;                  /**< Last bounded provider progress detail. */

        // Destination tab fields
        int namingConvention{0};            /**< 0=Preserve source name, 1=Lowercase+underscore, 2=AssetId prefix. */
        int subfolderByType{0};             /**< 0=Meshes/Textures/Audio, 1=Mirror source structure, 2=Flat. */
        int assetIdStrategy{0};             /**< 0=New GUID, 1=Stable hash. */
        bool createMetaSidecar{true};       /**< Create .meta sidecar alongside asset. */
        bool overwriteWithoutPrompt{false}; /**< Skip conflict check, overwrite existing. */
    };

    /** @brief Bounded snapshot of one import operation, safe for UI consumption. */
    struct AssetImportSnapshot {
        std::string operationId;  /**< Stable operation identity. */
        std::uint64_t revision{}; /**< Monotonic snapshot revision. */
        AssetImportPhase phase{AssetImportPhase::Selecting};
        std::vector<AssetImportItem> items; /**< All items in deterministic order. */
        std::size_t selectedItemIndex{0};   /**< Index of the selected item for settings panel. */
        bool canCommit{false};              /**< True when ReadyToCommit. */
        bool canCancel{false};              /**< True before Committing/Completed/Failed. */
    };

    // ---------------------------------------------------------------------------
    // Import request
    // ---------------------------------------------------------------------------

    /** @brief Typed request to start one import operation. */
    struct AssetImportRequest {
        std::filesystem::path projectRoot;              /**< Absolute project root. */
        std::vector<std::filesystem::path> sourceFiles; /**< Absolute paths to source files. */
        std::string destinationFolder;                  /**< Project-relative destination folder. */
    };

    // ---------------------------------------------------------------------------
    // Asset ID generator (injectable)
    // ---------------------------------------------------------------------------

    /** @brief Host-owned injectable asset identity generator. */
    class IAssetIdGenerator {
    public:
        virtual ~IAssetIdGenerator() = default;

        /**
         * @brief Generates one unique asset identity.
         * @return A new canonical AssetId.
         */
        [[nodiscard]] virtual AssetId Generate() = 0;
    };

    // ---------------------------------------------------------------------------
    // Asset import committer (host-owned)
    // ---------------------------------------------------------------------------

    /** @brief Validated batch ready for host commit. */
    struct PreparedAssetImportBatch {
        std::string operationId;
        std::filesystem::path projectRoot;
        ProjectPath destinationFolder;
        std::vector<AssetImportItem> items;
    };

    /**
     * @brief Host-owned commit boundary for asset import.
     * @details Acquires a project mutation lease, validates the batch against
     *          the current registry, assigns AssetIds, writes sidecars and
     *          editor payloads, and atomically publishes one registry snapshot.
     */
    class IAssetImportCommitter {
    public:
        virtual ~IAssetImportCommitter() = default;

        /**
         * @brief Commits a validated import batch to durable project storage.
         * @param batch Fully validated batch with items in deterministic order.
         * @param idGenerator Injected identity generator.
         * @param cancellation Cancellation token for cooperative abort.
         * @return Success or a typed commit failure. The registry is not modified on failure.
         */
        [[nodiscard]] virtual Result<void> Commit(const PreparedAssetImportBatch &batch, IAssetIdGenerator &idGenerator,
                                                  const CancellationToken &cancellation) = 0;
    };

    // ---------------------------------------------------------------------------
    // Asset import operation
    // ---------------------------------------------------------------------------

    /**
     * @brief Headless bounded import operation.
     * @details Accepts source files, selects importers from the pinned catalog snapshot,
     *          runs importer strategies through the job system, collects results,
     *          and produces a ReadyToCommit snapshot. The host committer owns
     *          the mutation boundary.
     *
     *          The operation never writes to the project, assigns AssetIds, or
     *          publishes registry snapshots. Importers receive only borrowed
     *          source bytes and host-owned output sinks.
     *
     *          Mutation methods and provider progress callbacks may run on a
     *          worker while Snapshot and Cancel run on the UI thread. State is
     *          synchronized internally, and no operation lock is held while
     *          invoking provider code. The operation must outlive that work.
     */
    class AssetImportOperation final {
    public:
        /**
         * @brief Constructs an operation pinned to one catalog snapshot.
         * @param jobs Job system that outlives this operation.
         * @param catalog Published immutable importer catalog snapshot.
         */
        AssetImportOperation(JobSystem &jobs, std::shared_ptr<const AssetImporterCatalogSnapshot> catalog);

        /**
         * @brief Starts the import operation.
         * @param request Typed import request.
         * @param cancellation Parent cancellation token.
         * @return Initial Selecting-phase snapshot.
         */
        [[nodiscard]] Result<AssetImportSnapshot> Start(const AssetImportRequest &request, const CancellationToken &cancellation);

        /** @brief Adds additional files to an existing operation queue. */
        [[nodiscard]] Result<AssetImportSnapshot> AddFiles(const std::vector<std::filesystem::path> &sourceFiles,
                                                           const std::filesystem::path &projectRoot, const CancellationToken &cancellation);

        /** @brief Imports a single item by index, running its importer strategy. */
        [[nodiscard]] Result<AssetImportSnapshot> ImportSingleItem(std::size_t index, const CancellationToken &cancellation);

        /**
         * @brief Replaces the serialized importer settings for one queued item.
         * @param index Queue index to update.
         * @param settings Settings keyed by importer descriptor ID.
         * @return Updated snapshot, or a typed error when @p index is invalid.
         */
        [[nodiscard]] Result<AssetImportSnapshot> SetItemSettings(std::size_t index, TransparentStringMap<std::string> settings);

        /**
         * @brief Returns the current snapshot for UI polling.
         * @return A consistent copy protected from concurrent worker updates.
         */
        [[nodiscard]] AssetImportSnapshot Snapshot() const;

        /**
         * @brief Cancels the operation.
         */
        void Cancel();

    private:
        /**
         * @brief Projects one bounded provider progress update into the published snapshot.
         * @param operationId Operation identity captured before provider entry.
         * @param index Queue index being imported.
         * @param completedUnits Completed provider work units.
         * @param totalUnits Non-zero total provider work units.
         * @param message Optional bounded provider phase detail.
         */
        void ReportProgress(std::string_view operationId, std::size_t index, std::uint64_t completedUnits, std::uint64_t totalUnits,
                            std::string_view message);

        [[maybe_unused]] JobSystem &jobs_;  // NOSONAR(cpp:S1068) Retained for asynchronous job execution parity
        std::shared_ptr<const AssetImporterCatalogSnapshot> catalog_;
        mutable std::mutex mutex_; /**< Protects snapshot_, revision_, and cancelled_ across worker/UI threads. */
        AssetImportSnapshot snapshot_;
        std::uint64_t revision_{0};
        bool cancelled_{false};
    };

}  // namespace Horo::Assets
