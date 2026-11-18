#pragma once
/** @file AssetSourcePublication.h
 * @brief Bounded canonical source reads and exclusive tracked durable
 * publication.
 */
#include "Horo/Assets/AssetId.h"
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

    /** @brief Exact source byte identity; missing differs from an existing empty
     * file. */
    struct AssetSourceIdentity final {
        bool exists{};
        std::size_t bytes{};
        Sha256Digest digest;
        auto operator<=>(const AssetSourceIdentity &) const = default;
    };

    /** @brief One bounded owned source read; no format interpretation or source
     * mutation. */
    struct AssetSourceCapture final {
        AssetSourceIdentity identity;
        std::vector<std::byte> bytes;
    };
    /** @brief Reports irreversible replacement independently from later directory
     * durability failure. */
    enum class AssetSourcePublicationStatus : std::uint8_t {
        Durable,
        ReconciliationRequired
    };

    /** @brief Successful replacement receipt; uncertain durability never marks a
     * document saved. */
    struct AssetSourcePublication final {
        AssetSourcePublicationStatus status{AssetSourcePublicationStatus::ReconciliationRequired};
        AssetSourceIdentity identity;
        std::optional<Error> durabilityError;
    };

    /** @brief Reads an absolute physically contained regular file with finite
     * allocation and cancellation.
     * @param root Canonical authorized project root. @param path Canonical absolute
     * destination.
     * @param maximumBytes Finite caller bound no larger than 64 MiB. @param
     * cancellation Cooperative observer.
     * @return Owned bytes and exact identity, missing identity, or typed
     * zero-mutation failure.
     */
    [[nodiscard]] Result<AssetSourceCapture> CaptureAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                                std::size_t maximumBytes, CancellationToken cancellation = {});
    /** @brief Returns the canonical exclusive publication lock path for one source.
     * @param root Authorized canonical root. @param path Canonical
     * project-contained destination.
     * @return Lock location or unsafe-path error; no lock or directory is created.
     */
    [[nodiscard]] Result<std::filesystem::path> AssetSourcePublicationLockPath(const std::filesystem::path &root,
                                                                               const std::filesystem::path &path);
    /** @brief Publishes exact bytes against an accepted source identity under a
     * real native exclusive lease.
     * @param root Authorized root. @param path Canonical destination. @param
     * expected Exact accepted source identity.
     * @param bytes Complete canonical bytes. @param lease Live lease protecting
     * AssetSourcePublicationLockPath.
     * @param files Native durable operations. @param cancellation Cooperative
     * observer.
     * @return Durable or reconciliation receipt, or failure before native
     * replacement.
     * @pre All cooperating source writers honor this lease and their shared project
     * mutation coordinator.
     * @details Staging and final source recheck happen while the lease is held.
     * Native rename is not a content CAS against noncooperating writes after the
     * final recheck; those writers must be excluded by the host's publication
     * authority. Escaping/retargeted symlinks fail. No force-overwrite mode exists.
     */
    [[nodiscard]] Result<AssetSourcePublication> PublishAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                                    const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                                    const ExclusiveFileLock &lease, DurableFileSystem &files,
                                                                    CancellationToken cancellation = {});

    /** @brief Allowed filesystem placement for the private attempt directory. */
    enum class AssetSourceAttemptPlacement : std::uint8_t {
        DestinationLocal, /**< Same filesystem as the canonical destination's parent. */
        ProjectLocal,     /**< Project-local authoring storage for destinations already on this filesystem. */
    };

    /** @brief Durable caller evidence identifying one private publication candidate.
     * @details The host supplies a fresh UUID per attempt. The full payload identity also binds retirement authority.
     */
    struct AssetSourcePublicationAttempt final {
        AssetId id;
        AssetSourceIdentity payload;
        AssetSourceAttemptPlacement placement{AssetSourceAttemptPlacement::DestinationLocal};
    };

    /** @brief Publishes through a fresh private attempt directory on the canonical destination's filesystem.
     * @param root Canonical project root. @param path Canonical destination.
     * @param expected Exact current canonical identity. @param bytes Complete replacement.
     * @param attempt Fresh host UUID and exact replacement identity. @param lease Exact live destination lease.
     * @param files Durable owner. @param cancellation Cooperative observer.
     * @return Tracked canonical publication receipt or a typed failure before canonical replacement.
     * @details The attempt is sealed before creating its payload. Ordinary scope exit retires owned staging.
     * A new attempt never reuses or overwrites an old stage. The caller may separately retire a known interrupted attempt.
     */
    [[nodiscard]] Result<AssetSourcePublication> PublishAssetSourceAttempt(
        const std::filesystem::path &root, const std::filesystem::path &path, const AssetSourceIdentity &expected,
        std::span<const std::byte> bytes, const AssetSourcePublicationAttempt &attempt, const ExclusiveFileLock &lease,
        DurableFileSystem &files, CancellationToken cancellation = {});

    /** @brief Retires an exact known attempt, preserving unknown namespace occupants and partial or changed payloads.
     * @param root Canonical project root. @param path Canonical destination.
     * @param attempt Exact recorded UUID and planned payload identity. @param lease Exact live destination lease.
     * @param files Durable owner.
     * @return Success for an absent or safely retired attempt, or a typed failure preserving foreign evidence.
     * @details Validates the immutable seal and a bounded single-link no-follow inventory before removal.
     * Never mutates the canonical source. One failed retirement does not authorize deletion of another attempt.
     */
    [[nodiscard]] Result<void> RetireAssetSourcePublicationAttempt(const std::filesystem::path &root, const std::filesystem::path &path,
                                                                   const AssetSourcePublicationAttempt &attempt,
                                                                   const ExclusiveFileLock &lease, DurableFileSystem &files);

    /** @brief Captures legacy reimport payloads with the existing import ceiling of
     * 1 GiB.
     * @param root Authorized canonical root. @param path Canonical contained
     * source. @param cancellation Cooperative observer.
     * @return Owned original bytes and identity; release the capture before
     * allocating upstream import buffers.
     * @details Uses the same source admission as CaptureAssetSource; its default 64
     * MiB ceiling remains unchanged.
     */
    [[nodiscard]] Result<AssetSourceCapture> CaptureReimportAssetSource(const std::filesystem::path &root,
                                                                        const std::filesystem::path &path,
                                                                        CancellationToken cancellation = {});

    /** @brief Streams the identity of a reimport source up to 1 GiB using fixed 4
     * KiB scratch storage.
     * @param root Authorized canonical root. @param path Canonical contained
     * source. @param cancellation Cooperative observer.
     * @return Exact observed identity, missing identity, or typed
     * admission/read/cancellation failure.
     * @details Applies the same size, modification-time and containment fences as
     * owned captures. Cooperating writers must hold the canonical source lease;
     * this read is not an atomic snapshot of noncooperating writes.
     */
    [[nodiscard]] Result<AssetSourceIdentity> InspectReimportAssetSource(const std::filesystem::path &root,
                                                                         const std::filesystem::path &path,
                                                                         CancellationToken cancellation = {});

    /** @brief Publishes a legacy reimport payload through the canonical tracked
     * publication primitive, bounded to 1 GiB.
     * @param root Authorized root. @param path Canonical destination. @param
     * expected Original admitted identity.
     * @param bytes Complete imported payload. @param lease Exact source lease
     * retained across admission and publication.
     * @param files Host durable authority. @param cancellation Cooperative
     * observer.
     * @return Durable or reconciliation receipt, or failure before native
     * replacement.
     * @details Identical authority, staging, compensation-receipt and
     * noncooperating-writer limitations to PublishAssetSource. Original and staged
     * identity rechecks stream bounded scratch instead of retaining duplicate
     * payload buffers.
     */
    [[nodiscard]] Result<AssetSourcePublication> PublishReimportAssetSource(
        const std::filesystem::path &root, const std::filesystem::path &path, const AssetSourceIdentity &expected,
        std::span<const std::byte> bytes, const ExclusiveFileLock &lease, DurableFileSystem &files, CancellationToken cancellation = {});
}  // namespace Horo::Assets
