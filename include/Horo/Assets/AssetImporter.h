#pragma once

/**
 * @file AssetImporter.h
 * @brief Host-owned immutable importer contribution catalog, declarative settings, and sealed lookup.
 */

#include "Horo/Assets/AssetPreview.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::Assets {

    // ---------------------------------------------------------------------------
    // Declarative import settings
    // ---------------------------------------------------------------------------

    /** @brief Kind of a declarative import setting field. */
    enum class ImportSettingKind : std::uint8_t {
        Boolean,
        Integer,
        Float,
        Text,
        Choice,
    };

    /** @brief Typed value for one import setting. */
    using ImportSettingValue = std::variant<bool, std::int64_t, double, std::string, std::size_t>;

    /** @brief Borrowed host progress callback for one synchronous import invocation. */
    struct AssetImportProgressSink {
        void *context{}; /**< Opaque host context valid only during the import call. */
        void (*report)(void *context, std::uint64_t completedUnits, std::uint64_t totalUnits,
                       std::string_view message){}; /**< Non-owning callback; providers must not retain it. */

        /**
         * @brief Reports bounded progress to the host when a callback is installed.
         * @param completedUnits Completed work units, not greater than @p totalUnits.
         * @param totalUnits Non-zero total work units.
         * @param message Optional human-readable phase detail.
         */
        void Report(std::uint64_t completedUnits, std::uint64_t totalUnits, std::string_view message = {}) const noexcept {
            if (report != nullptr)
                report(context, completedUnits, totalUnits, message);
        }
    };

    /** @brief One choice option in a Choice setting. */
    struct ImportSettingChoice {
        std::string id;           /**< Stable choice identifier. */
        std::string labelKey;     /**< Localization key for the choice label. */
        ImportSettingValue value; /**< The value this choice represents. */
    };

    /** @brief Declarative descriptor for one import setting field. */
    struct ImportSettingDescriptor {
        std::string id;                           /**< Stable setting identifier. */
        std::string labelKey;                     /**< Localization key for the field label. */
        std::string descriptionKey;               /**< Localization key for the field description. */
        ImportSettingKind kind;                   /**< Value type. */
        ImportSettingValue defaultValue;          /**< Default value for this setting. */
        std::optional<double> minimum;            /**< Minimum value for Integer/Float. */
        std::optional<double> maximum;            /**< Maximum value for Integer/Float. */
        std::vector<ImportSettingChoice> choices; /**< Choices for Choice kind. */
        bool includeInPresets{false};             /**< Whether user-created import presets may retain this field. */
    };

    // ---------------------------------------------------------------------------
    // Import input and output
    // ---------------------------------------------------------------------------

    /** @brief Borrowed immutable input for one import invocation. */
    struct AssetImportInput {
        std::span<const std::uint8_t> sourceBytes; /**< Borrowed source bytes. Valid for the invocation only. */
        std::string_view sourceExtension;          /**< Lowercase file extension without dot. */
        std::vector<ImportSettingValue> settings;  /**< Resolved setting values in descriptor order. */
        AssetImportProgressSink progress;          /**< Optional host-owned progress projection. */
    };

    /** @brief Diagnostic produced during import. */
    struct ImportDiagnostic {
        enum class Severity : std::uint8_t {
            Info,
            Warning,
            Error,
        };

        Severity severity;       /**< Diagnostic severity. */
        std::string code;        /**< Stable error code. */
        std::string message;     /**< Human-readable message. */
        std::optional<int> line; /**< Source line when available. */
    };

    /** @brief One asset result produced by an importer. */
    struct PreparedAssetImport {
        AssetTypeId type;                          /**< Asset type identity. */
        std::vector<std::uint8_t> editorPayload;   /**< Canonical editor payload bytes. */
        std::vector<AssetId> dependencies;         /**< Asset identity dependencies. */
        std::vector<ImportDiagnostic> diagnostics; /**< Per-asset diagnostics. */
    };

    // ---------------------------------------------------------------------------
    // Importer contribution
    // ---------------------------------------------------------------------------

    /**
     * @brief Abstract importer strategy.
     * @details Both built-in importers and C-ABI adapters implement this interface.
     *          Import is background/tooling work, never in a frame-hot path.
     */
    class IAssetImporter {
    public:
        virtual ~IAssetImporter() = default;

        /**
         * @brief Import a source asset into its editor representation.
         * @param input Borrowed immutable source bytes, extension, and resolved settings.
         * @param cancellation Host-owned cancellation token.
         * @return Prepared editor payload and diagnostics, or a typed error.
         */
        [[nodiscard]] virtual Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                                 const CancellationToken &cancellation) const = 0;
    };

    /**
     * @brief Immutable contribution descriptor registered in the catalog.
     */
    struct AssetImporterContribution {
        std::string contributionId;              /**< Stable unique contribution identity. */
        std::string packageId;                   /**< Owning package identity. */
        std::string moduleId;                    /**< Module identity. */
        std::string moduleVersion;               /**< Canonical semantic version of the owning module. */
        std::string version;                     /**< Canonical semantic version of this contribution. */
        std::vector<std::string> fileExtensions; /**< Lowercase extensions without dot. */
        std::vector<AssetTypeId> assetTypes;     /**< Asset types this importer produces. */

        /** @brief Optional destination subfolder category for auto-organization. Empty = no subfolder. */
        std::string subfolderCategory; /**< e.g. "Meshes", "Textures", "Audio". */

        std::vector<ImportSettingDescriptor> settings;                         /**< Declarative import settings schema. */
        bool builtIn{false};                                                   /**< True for engine-provided importers. */
        std::shared_ptr<const IAssetImporter> strategy;                        /**< Immutable strategy instance. */
        std::shared_ptr<const IAssetPreviewProvider> previewProvider;          /**< Optional editor-card preview strategy. */
        AssetPreviewFallback previewFallback{AssetPreviewFallback::Automatic}; /**< Host fallback when preview fails. */

        /** @brief Target file extension this importer produces (including dot). */
        std::string targetExtension{".horoasset"};

        /** @brief True if this importer supports creating a meta sidecar. */
        bool supportsMetaSidecar{true};

        /** @brief Returns true when this contribution handles the given extension. */
        [[nodiscard]] bool HandlesExtension(std::string_view extension) const noexcept;
    };

    // ---------------------------------------------------------------------------
    // Importer catalog
    // ---------------------------------------------------------------------------

    /**
     * @brief Immutable sealed catalog snapshot.
     * @details Entries are sorted by extension, then contribution ID.
     *          Lookup is allocation-free on the pinned snapshot.
     */
    class AssetImporterCatalogSnapshot final {
    public:
        AssetImporterCatalogSnapshot() = default;

        explicit AssetImporterCatalogSnapshot(std::vector<AssetImporterContribution> entries) : entries_(std::move(entries)) {}

        /**
         * @brief Finds the importer for the given file extension.
         * @param extension Lowercase extension without dot.
         * @return Strategy pointer, or nullptr when no matching contribution exists.
         * @details When multiple contributions claim the same extension, the first
         *          unambiguous built-in is returned. Resolution by project policy
         *          requires an explicit contribution ID lookup.
         */
        [[nodiscard]] const IAssetImporter *FindByExtension(std::string_view extension) const noexcept;

        /**
         * @brief Finds a contribution by its stable identity.
         * @param contributionId Exact contribution ID to look up.
         * @return Contribution pointer, or nullptr when not found.
         */
        [[nodiscard]] const AssetImporterContribution *FindById(std::string_view contributionId) const noexcept;

        /** @brief Finds a contribution by file extension. */
        [[nodiscard]] const AssetImporterContribution *FindContributionByExtension(std::string_view extension) const noexcept;

        /**
         * @brief Finds the preview contribution for a stable asset type.
         * @param assetType Stable type to present.
         * @return First deterministic matching contribution, or null when no importer owns the type.
         */
        [[nodiscard]] const AssetImporterContribution *FindPreviewContribution(const AssetTypeId &assetType) const noexcept;

    private:
        friend class AssetImporterCatalog;

        std::vector<AssetImporterContribution> entries_;
    };

    /**
     * @brief Host-owned mutable candidate builder for transactionally registering importer contributions.
     * @details Build a candidate, validate it, and publish a new immutable snapshot atomically.
     *          The catalog is sealed after the first publish.
     */
    class AssetImporterCatalog final {
    public:
        AssetImporterCatalog();
        ~AssetImporterCatalog(); /**< Required for PIMPL destructor visibility. */

        /**
         * @brief Registers an importer contribution into the candidate.
         * @param entry Contribution to register.
         * @return Result<void> with a typed error on duplicate ID or extension conflict.
         */
        [[nodiscard]] Result<void> Register(AssetImporterContribution entry);

        /**
         * @brief Validates a complete contribution batch without mutating the catalog candidate.
         * @param entries Borrowed candidate entries.
         * @return Success when RegisterBatch can admit the batch against the current catalog state.
         */
        [[nodiscard]] Result<void> ValidateBatch(std::span<const AssetImporterContribution> entries) const;

        /**
         * @brief Registers a complete contribution batch atomically.
         * @param entries Candidate entries copied into the catalog only when every entry validates.
         * @return Success, or the first typed validation/conflict error with the catalog unchanged.
         */
        [[nodiscard]] Result<void> RegisterBatch(std::vector<AssetImporterContribution> entries);

        /**
         * @brief Seals the catalog and publishes the immutable snapshot.
         * @return The new snapshot, or a typed error when already sealed or validation fails.
         */
        [[nodiscard]] Result<std::shared_ptr<const AssetImporterCatalogSnapshot>> Publish();

        /**
         * @brief Returns the current published snapshot, or nullptr when not yet published.
         */
        [[nodiscard]] std::shared_ptr<const AssetImporterCatalogSnapshot> Snapshot() const noexcept;

        /**
         * @brief Returns true when the catalog has been sealed.
         */
        [[nodiscard]] bool IsSealed() const noexcept;

        /**
         * @brief Clears the unsealed candidate entries without publishing.
         */
        void Reset();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

}  // namespace Horo::Assets
