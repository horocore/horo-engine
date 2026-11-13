#pragma once
/** @file AssetReimportPublication.h @brief Private original-generation reimport admission and pair compensation. */
#include "Horo/Assets/AssetReimport.h"
#include "Horo/Assets/AssetSourcePublication.h"

namespace Horo::Assets::Detail {
    /** @brief One originally captured target and its exclusively created rollback copy. */
    struct ReimportEntry final {
        std::filesystem::path target;
        std::filesystem::path backup;
        AssetSourceIdentity original;
        AssetSourceIdentity promoted;
        std::optional<ExclusiveFileLock> lease;
        bool backupCreated{};
        bool published{};
    };

    /** @brief Synchronous owner retaining both original native writer leases through publication/compensation and cleanup. */
    class ReimportAdmission final {
    public:
        ReimportAdmission() = default;
        ReimportAdmission(ReimportAdmission &&) noexcept = default;
        ReimportAdmission &operator=(ReimportAdmission &&) = delete;
        ReimportAdmission(const ReimportAdmission &) = delete;
        ReimportAdmission &operator=(const ReimportAdmission &) = delete;
        ~ReimportAdmission();

        std::filesystem::path root;
        AssetImportMetadata metadata;
        const AssetImporterContribution *contribution{}; /**< Immutable request catalog borrow, never retained beyond this call. */
        DurableFileSystem *files{};
        std::vector<ReimportEntry> entries; /**< Payload then identity sidecar; identical writer acquisition/publication order. */
        bool committed{};
        bool uncertain{};
    };

    /** @brief Capture original payload/metadata under both native leases, parse captured metadata, validate destination and preserve
     * originals. */
    [[nodiscard]] Result<ReimportAdmission> AdmitReimport(const AssetReimportRequest &request, const CancellationToken &cancellation);
    /** @brief Publish expected originals and atomically commit registry visibility; compensate only our own promoted bytes on precommit
     * failure. */
    [[nodiscard]] Result<void> PublishReimport(ReimportAdmission &admission, PreparedAssetImport prepared,
                                               std::string_view serializedMetadata, AssetRegistry &registry,
                                               const CancellationToken &cancellation);
}  // namespace Horo::Assets::Detail
