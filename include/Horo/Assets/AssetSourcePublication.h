#pragma once
/** @file AssetSourcePublication.h
 * @brief Bounded canonical source reads and exclusive tracked durable publication.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"

#include <optional>
#include <vector>

namespace Horo::Assets {
    namespace AssetSourceErrors {
        extern const ErrorCodeDescriptor InvalidPath;
        extern const ErrorCodeDescriptor ReadFailed;
        extern const ErrorCodeDescriptor Limit;
        extern const ErrorCodeDescriptor Conflict;
        extern const ErrorCodeDescriptor Authority;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor DurabilityUnconfirmed;
    }  // namespace AssetSourceErrors

    /** @brief Exact source byte identity; missing differs from an existing empty file. */
    struct AssetSourceIdentity final {
        bool exists{};
        std::size_t bytes{};
        Sha256Digest digest;
        auto operator<=>(const AssetSourceIdentity &) const = default;
    };

    /** @brief One bounded owned source read; no format interpretation or source mutation. */
    struct AssetSourceCapture final {
        AssetSourceIdentity identity;
        std::vector<std::byte> bytes;
    };
    /** @brief Reports irreversible replacement independently from later directory durability failure. */
    enum class AssetSourcePublicationStatus : std::uint8_t {
        Durable,
        ReconciliationRequired
    };

    /** @brief Successful replacement receipt; uncertain durability never marks a document saved. */
    struct AssetSourcePublication final {
        AssetSourcePublicationStatus status{AssetSourcePublicationStatus::ReconciliationRequired};
        AssetSourceIdentity identity;
        std::optional<Error> durabilityError;
    };

    /** @brief Reads an absolute physically contained regular file with finite allocation and cancellation.
     * @param root Canonical authorized project root. @param path Canonical absolute destination.
     * @param maximumBytes Finite caller bound no larger than 64 MiB. @param cancellation Cooperative observer.
     * @return Owned bytes and exact identity, missing identity, or typed zero-mutation failure.
     */
    [[nodiscard]] Result<AssetSourceCapture> CaptureAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                                std::size_t maximumBytes, CancellationToken cancellation = {});
    /** @brief Returns the canonical exclusive publication lock path for one source.
     * @param root Authorized canonical root. @param path Canonical project-contained destination.
     * @return Lock location or unsafe-path error; no lock or directory is created.
     */
    [[nodiscard]] Result<std::filesystem::path> AssetSourcePublicationLockPath(const std::filesystem::path &root,
                                                                               const std::filesystem::path &path);
    /** @brief Publishes exact bytes against an accepted source identity under a real native exclusive lease.
     * @param root Authorized root. @param path Canonical destination. @param expected Exact accepted source identity.
     * @param bytes Complete canonical bytes. @param lease Live lease protecting AssetSourcePublicationLockPath.
     * @param files Native durable operations. @param cancellation Cooperative observer.
     * @return Durable or reconciliation receipt, or failure before native replacement.
     * @pre All cooperating source writers honor this lease and their shared project mutation coordinator.
     * @details Staging and final source recheck happen while the lease is held. Native rename is not a
     * content CAS against noncooperating writes after the final recheck; those writers must be excluded
     * by the host's publication authority. Escaping/retargeted symlinks fail. No force-overwrite mode exists.
     */
    [[nodiscard]] Result<AssetSourcePublication> PublishAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                                    const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                                    const ExclusiveFileLock &lease, DurableFileSystem &files,
                                                                    CancellationToken cancellation = {});
}  // namespace Horo::Assets
