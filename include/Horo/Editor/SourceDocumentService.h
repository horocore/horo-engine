#pragma once

/** @file SourceDocumentService.h
 * @brief Widget-independent editable source ownership and immutable UTF-8 snapshots.
 */

#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Foundation/CancellationToken.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace Horo::Editor {
    namespace SourceDocumentErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor TooLarge;
        extern const ErrorCodeDescriptor Binary;
        extern const ErrorCodeDescriptor Encoding;
        extern const ErrorCodeDescriptor Removed;
        extern const ErrorCodeDescriptor ReadFailed;
        extern const ErrorCodeDescriptor DiskChanged;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor WrongThread;
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace SourceDocumentErrors

    /** @brief Exact newline families present in a UTF-8 byte sequence; no normalization occurs. */
    enum class SourceNewlines : std::uint8_t {
        None,
        Lf,
        CrLf,
        Cr,
        Mixed
    };
    /** @brief Last explicit disk inspection; errors preserve both current buffer and original base. */
    enum class SourceExternalState : std::uint8_t {
        InSync,
        Changed,
        Removed,
        Unreadable
    };

    /** @brief Validated encoding metadata; UTF-16/32 and malformed UTF-8 are rejected. */
    struct SourceTextMetadata final {
        bool utf8Bom{}; /**< A leading UTF-8 BOM is retained in the owned bytes. */
        SourceNewlines newlines{SourceNewlines::None};
        auto operator<=>(const SourceTextMetadata &) const = default;
    };

    /** @brief Finite owner reservations. Detached readers retain their own snapshot leases. */
    struct SourceDocumentLimits final {
        std::size_t maximumDocumentBytes{8 * 1024 * 1024};
        std::size_t maximumResidentBytes{64 * 1024 * 1024}; /**< Owner text/base bytes, including replacement overlap. */
        std::size_t maximumDocuments{64};
    };

    namespace Detail {
        struct SourceDocumentRoot;
    }

    /** @brief Owned immutable read lease, safe on any thread after synchronized handoff.
     * @details Views remain valid while this snapshot (or a copy) lives. Neither edits, reload,
     * close, widget destruction nor service destruction change its bytes or metadata.
     * A moved-from/default snapshot is inert; it has revision zero and empty text.
     */
    class SourceDocumentSnapshot final {
    public:
        SourceDocumentSnapshot() = default;
        /** @brief Returns exact session identity. @return Empty identity for an inert snapshot. */
        [[nodiscard]] DocumentIdentity Identity() const;
        /** @brief Returns the monotonic committed observation/edit revision. @return Zero when inert. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Returns the revision of the last explicitly loaded disk base. @return Zero when inert. */
        [[nodiscard]] std::uint64_t BaseRevision() const noexcept;
        /** @brief Returns exact validated current bytes. @return Read-only call-independent borrowed view. */
        [[nodiscard]] std::string_view Text() const noexcept;
        /** @brief Returns exact last explicitly loaded disk bytes. @return Read-only base view. */
        [[nodiscard]] std::string_view DiskBase() const noexcept;
        /** @brief Compares current authored bytes to the saved base, not notification revisions. @return False when equal or inert. */
        [[nodiscard]] bool Dirty() const noexcept;
        /** @brief Returns current encoding/newline evidence. @return Default metadata when inert. */
        [[nodiscard]] SourceTextMetadata Metadata() const noexcept;
        /** @brief Returns saved-base encoding/newline evidence. @return Default metadata when inert. */
        [[nodiscard]] SourceTextMetadata BaseMetadata() const noexcept;
        /** @brief Returns last explicit disk inspection disposition. @return Unreadable when inert. */
        [[nodiscard]] SourceExternalState ExternalState() const noexcept;

    private:
        friend class SourceDocumentService;
        explicit SourceDocumentSnapshot(std::shared_ptr<const Detail::SourceDocumentRoot> root) noexcept;
        std::shared_ptr<const Detail::SourceDocumentRoot> root_;
    };

    /** @brief Revision-fenced semantic byte-range edit; offsets may not split a UTF-8 scalar or BOM. */
    struct SourceTextEdit final {
        std::uint64_t expectedRevision{};
        std::size_t offset{};
        std::size_t eraseBytes{};
        std::string_view insert; /**< Synchronously borrowed, copied before publication; never retained. */
    };

    /** @brief Single-owner-thread source sessions; no widget, renderer, save publication or ambient activation.
     * @details Open/InspectDisk/Reload are explicit bounded loading operations, never draw-loop work.
     * Edits prepare a complete validated replacement before publishing once. All service calls and moves
     * stay on the creating thread; immutable snapshots alone cross threads. No jobs or locks are owned.
     * Historical reader leases are caller-owned and must be bounded by the consuming service.
     */
    class SourceDocumentService final {
    public:
        /** @brief Creates a project-contained source owner. @param projectRoot Explicit existing authorized directory.
         * @param limits Finite limits no larger than defaults. @throws std::bad_alloc On owner allocation failure.
         */
        explicit SourceDocumentService(const std::filesystem::path &projectRoot, SourceDocumentLimits limits = {});
        ~SourceDocumentService();
        SourceDocumentService(SourceDocumentService &&) noexcept;
        SourceDocumentService &operator=(SourceDocumentService &&) noexcept;
        SourceDocumentService(const SourceDocumentService &) = delete;
        SourceDocumentService &operator=(const SourceDocumentService &) = delete;
        /** @brief Admits an exact Source identity and loads its bounded validated bytes, or focuses an existing session.
         * @param identity Registry-issued Source identity; absolutePath must match its canonical project-relative key.
         * @param absolutePath Canonical project-contained file. @param cancellation Cooperative observer.
         * @return Immutable current snapshot or typed failure, without partial admission.
         */
        [[nodiscard]] Result<SourceDocumentSnapshot> Open(const DocumentIdentity &identity, const std::filesystem::path &absolutePath,
                                                          CancellationToken cancellation = {});
        /** @brief Queries an owner session without I/O. @param instance Exact session identity.
         * @return Snapshot or typed unknown/closed/thread failure.
         */
        [[nodiscard]] Result<SourceDocumentSnapshot> Snapshot(DocumentInstanceId instance) const;
        /** @brief Applies one complete semantic edit. @param instance Exact session. @param edit Revision-fenced byte patch.
         * @param cancellation Cooperative observer. @return Snapshot or typed failure; failed/no-op edits do not advance revision.
         */
        [[nodiscard]] Result<SourceDocumentSnapshot> Edit(DocumentInstanceId instance, const SourceTextEdit &edit,
                                                          CancellationToken cancellation = {});
        /** @brief Explicitly checks disk without replacing local text or its base. @param instance Exact session.
         * @param cancellation Cooperative observer. @return Snapshot or typed disk error; error disposition is queryable afterward.
         */
        [[nodiscard]] Result<SourceDocumentSnapshot> InspectDisk(DocumentInstanceId instance, CancellationToken cancellation = {});
        /** @brief Explicitly discards local text and loads a new validated disk base; never implicit watcher mutation.
         * @param instance Exact session. @param expectedRevision Exact current observation/edit revision.
         * @param cancellation Cooperative observer. @return Clean new snapshot or failure preserving prior text/base.
         */
        [[nodiscard]] Result<SourceDocumentSnapshot> Reload(DocumentInstanceId instance, std::uint64_t expectedRevision,
                                                            CancellationToken cancellation = {});
        /** @brief Releases one owner session; detached snapshots remain alive. @param instance Exact session.
         * @return Success or typed failure. Closing dirty sessions requires the host's explicit discard/save decision.
         */
        [[nodiscard]] Result<void> Close(DocumentInstanceId instance);
        /** @brief Closes admission and releases all owner roots without invalidating detached readers.
         * @return Success or wrong-thread failure; repeated shutdown is idempotent.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        /** @brief Checks owner-thread lifecycle before accessing service storage. */
        [[nodiscard]] Result<void> CheckAccess() const;
        struct Storage;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Editor
