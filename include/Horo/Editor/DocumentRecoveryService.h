#pragma once
/** @file DocumentRecoveryService.h @brief Shared bounded recovery storage, independent from canonical source authority. */
#include "Horo/Assets/AssetId.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Editor/ProjectMutation.h"

namespace Horo::Editor {
    /** @brief Shared finite retention policy; no implicit canonical source replacement or eviction. */
    struct DocumentRecoveryLimits final {
        std::size_t maximumRecordBytes{8 * 1024 * 1024};
        std::size_t maximumRecords{64};
        std::size_t maximumTotalBytes{128 * 1024 * 1024};
    };

    /** @brief Shared checksum-protected recovery envelope; payload uses the document's sole source codec. */
    struct DocumentRecoveryRecord final {
        Assets::AssetId asset;
        DocumentOpenKey document;
        Assets::AssetId sessionOwner;
        std::uint64_t sessionGeneration{};
        std::uint64_t revision{};
        std::uint64_t capturedAtUnixSeconds{};
        Assets::AssetSourceIdentity acceptedSource;
        Sha256Digest state;
        std::vector<std::byte> payload;
        [[nodiscard]] bool operator==(const DocumentRecoveryRecord &) const = default;
    };

    /** @brief Shared record owner; writes never target the canonical document path.
     * @details One recovery slot per stable asset/open key; count/bytes are bounded and full slots reject.
     * Newer captures in the same session cannot be replaced by a late older capture. Different sessions
     * must explicitly discard an existing candidate before writing. Reads are untrusted and read-only.
     */
    class DocumentRecoveryService final {
    public:
        /** @brief Borrows the existing project writer and durability owners.
         * @param root Authorized canonical root. @param mutations Shared project authority.
         * @param files Tracked durability implementation. @param limits Finite retention budgets.
         */
        DocumentRecoveryService(std::filesystem::path root, const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                DocumentRecoveryLimits limits = {});
        /** @brief Inspects a candidate without modifying source or recovery.
         * @param asset Exact asset identity. @param key Exact persistent document key.
         * @return Validated read-only envelope, absence, or bounded corruption/read error.
         */
        [[nodiscard]] Result<std::optional<DocumentRecoveryRecord>> Inspect(Assets::AssetId asset, const DocumentOpenKey &key) const;
        /** @brief Writes one exact immutable capture through shared quota/native publication.
         * @param record Complete metadata and existing source-codec payload. @param cancellation Cooperative observer.
         * @return Success only after durable recovery publication; canonical source is never written.
         */
        [[nodiscard]] Result<void> Write(const DocumentRecoveryRecord &record, CancellationToken cancellation = {});
        /** @brief Transfers a recovery slot after an explicit acceptance without deleting the prior durable candidate first.
         * @param record New owner's complete capture. @param accepted Exact candidate explicitly accepted by that owner.
         * @param cancellation Cooperative observer. @return Durable replacement or error preserving the prior candidate before commit.
         */
        [[nodiscard]] Result<void> ReplaceAccepted(const DocumentRecoveryRecord &record, const DocumentRecoveryRecord &accepted,
                                                   CancellationToken cancellation = {});
        /** @brief Discards only an exact inspected recovery candidate after an explicit decision.
         * @param record Exact candidate including session/revision/state and payload checksum.
         * @return Success or stale/authority/durability error; a newer candidate survives.
         */
        [[nodiscard]] Result<void> Discard(const DocumentRecoveryRecord &record);

    private:
        [[nodiscard]] Result<void> WriteRecord(const DocumentRecoveryRecord &record, const DocumentRecoveryRecord *accepted,
                                               CancellationToken cancellation);
        [[nodiscard]] Result<std::filesystem::path> Path(Assets::AssetId asset, const DocumentOpenKey &key) const;
        std::filesystem::path root_;
        const ProjectMutationCoordinator &mutations_;
        DurableFileSystem &files_;
        DocumentRecoveryLimits limits_;
    };

    /** @brief Publishes one opaque recovery record beneath the shared recovery namespace.
     * @param root Canonical project root. @param destination Canonical path beneath .horo/local/recovery.
     * @param bytes Bounded owning document's recovery envelope; source codecs remain domain-owned.
     * @param expected Exact existing recovery bytes identity captured under the shared project lease.
     * @param files Tracked durable service. @param limits Shared retention budgets.
     * @param cancellation Cooperative observer. @return Tracked receipt or zero-replacement failure.
     * @pre Caller retains ProjectMutationCoordinator authority for the complete operation.
     * @details Recovery quota rejects new records; it never silently removes a different document's candidate.
     */
    [[nodiscard]] Result<Assets::AssetSourcePublication> PublishDocumentRecovery(
        const std::filesystem::path &root, const std::filesystem::path &destination, std::span<const std::byte> bytes,
        const Assets::AssetSourceIdentity &expected, DurableFileSystem &files, DocumentRecoveryLimits limits = {},
        CancellationToken cancellation = {});
}  // namespace Horo::Editor
