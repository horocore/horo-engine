#pragma once

/**
 * @file UpdateOfflineSource.h
 * @brief Managed local update sources and authenticated offline package import.
 */

#include "Horo/Release/UpdateDiscovery.h"
#include "Horo/Release/UpdateDownloadSession.h"
#include "Horo/Release/UpdateStageReady.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Release {
    /** @brief Managed source medium; an enterprise mirror is a locally mounted, administrator-synchronized mirror. */
    enum class UpdateOfflineSourceKind : std::uint8_t {
        LocalDirectory,
        RemovableMedia,
        EnterpriseMirror
    };

    /** @brief Inert administrator-configured source, with lower precedence values tried first. */
    struct UpdateOfflineSource final {
        UpdateOfflineSourceKind kind{UpdateOfflineSourceKind::LocalDirectory};
        std::string sourceId;
        std::filesystem::path root;
        std::string channel;
        std::uint32_t precedence{};
        std::uint64_t maximumExpiredManifestSeconds{};
        bool allowDowngrade{};
    };

    /**
     * @brief Validates unique managed identities and returns their stable precedence order.
     * @param sources Host-owned configured source descriptors.
     * @return Ordered pointers into sources, or a source-policy error.
     */
    [[nodiscard]] Result<std::vector<const UpdateOfflineSource *>> OrderUpdateOfflineSources(
        const std::vector<UpdateOfflineSource> &sources);

    /**
     * @brief Allows source precedence fallback only for missing media or metadata, never for rejected evidence.
     * @param failure Failure returned by one managed offline source.
     * @return True only when the next configured source may be tried.
     */
    [[nodiscard]] bool MayTryNextOfflineSource(const Error &failure) noexcept;

    /** @brief Complete verified offline import result for subsequent activation. */
    struct ImportedOfflineUpdate final {
        UpdatePackageRecord package;
        UpdateTransferCheckpoint checkpoint;
        std::filesystem::path readyMarker;
    };

    /** @brief Host-owned inputs for one local import; the source and private parent remain quiescent during the call. */
    struct UpdateOfflineImportRequest final {
        const UpdateOfflineSource &source;
        const UpdateAdmissionContext &admission;
        const UpdateTrustRootSnapshot &roots;
        const UpdateDownloadPaths &privatePaths;
        const std::filesystem::path &stageRoot;
        const UpdateDownloadLimits &downloadLimits;
        const UpdateArchiveLimits &archiveLimits;
    };

    /**
     * @brief Reads a canonical signed manifest and digest-named package from one configured root, then stages it.
     * @param request Source, installed-product policy, trust roots, and private staging paths.
     * @param files Durable private filesystem kept alive for the operation.
     * @param provider Trusted signature provider for both manifest and package.
     * @param cancellation Cooperative cancellation before ready publication.
     * @return Authenticated package, durable checkpoint, and ready marker, or a typed failure.
     * @note The host serializes source mutation and private staging. A missing source may be retried at the next
     * precedence; any signature, policy, path, or package failure must stop fallback.
     */
    [[nodiscard]] Result<ImportedOfflineUpdate> ImportOfflineZipUpdate(const UpdateOfflineImportRequest &request,
                                                                       NativeDurableFileSystem &files,
                                                                       std::shared_ptr<const Security::SignatureProvider> provider,
                                                                       CancellationToken cancellation);

    /**
     * @brief Imports a signed Linux portable tar.gz through the same source and private-stage policy as offline ZIP.
     * @param request Source, installed-product policy, trust roots, and private staging paths.
     * @param files Durable private filesystem kept alive for the operation.
     * @param provider Trusted signature provider for both manifest and package.
     * @param cancellation Cooperative cancellation before ready publication.
     * @return Authenticated package, durable checkpoint, and ready marker, or a typed failure.
     * @note The host serializes source mutation and private staging. Only an authenticated Linux tar.gz selection is admitted.
     */
    [[nodiscard]] Result<ImportedOfflineUpdate> ImportOfflineTarGzipUpdate(const UpdateOfflineImportRequest &request,
                                                                           NativeDurableFileSystem &files,
                                                                           std::shared_ptr<const Security::SignatureProvider> provider,
                                                                           CancellationToken cancellation);
}  // namespace Horo::Release
