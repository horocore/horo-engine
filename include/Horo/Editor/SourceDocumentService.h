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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo {
    class DurableFileSystem;
}

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
        extern const ErrorCodeDescriptor SaveConflict;
        extern const ErrorCodeDescriptor SaveOutcomeUnknown;
        extern const ErrorCodeDescriptor Busy;
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
        std::size_t maximumHistoryBytes{2 * 1024 * 1024}; /**< Per document; oversized edits clear history. */
        std::size_t maximumHistoryEntries{256};
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
        /** @brief Checks exact immutable published-root identity without copying paths or comparing source bytes.
         * @param other Retained publication to compare. @return True for the same root, including two inert snapshots.
         * @details This is not semantic text equality. Both leases retain their roots throughout comparison, preventing
         *          address reuse from identifying another publication as the same root. */
        [[nodiscard]] bool SharesStateWith(const SourceDocumentSnapshot &other) const noexcept;
        /** @brief Returns the monotonic committed observation/edit revision. @return Zero when inert. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Returns the revision of the last explicitly loaded or durably saved disk base. @return Zero when inert. */
        [[nodiscard]] std::uint64_t BaseRevision() const noexcept;
        /** @brief Returns exact validated current bytes. @return Read-only call-independent borrowed view. */
        [[nodiscard]] std::string_view Text() const noexcept;
        /** @brief Returns exact last explicitly loaded or durably saved disk bytes. @return Read-only base view. */
        [[nodiscard]] std::string_view DiskBase() const noexcept;
        /** @brief Compares current authored bytes to the saved base, not notification revisions. @return False when equal and durability is
         * confirmed, or inert. */
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

    /** @brief Host-issued editor-view identity, scoped to a project session; zero denotes a non-view edit. */
    struct SourceEditingViewId final {
        std::uint64_t value{};
        auto operator<=>(const SourceEditingViewId &) const = default;
    };

    /** @brief View-local caret/anchor byte offsets and logical scroll position captured around a semantic edit. */
    struct SourceEditingView final {
        std::size_t anchor{};
        std::size_t caret{};
        float scrollLines{};
        SourceEditingViewId viewId{}; /**< History metadata only; never a canonical document selection. */
        auto operator<=>(const SourceEditingView &) const = default;
    };

    /** @brief Why history cannot reach an earlier document state; the host presents this limitation. */
    enum class SourceHistoryBoundary {
        None,
        Evicted,
        OversizedEdit
    };

    /** @brief Bounded history availability for one exact admitted document. */
    struct SourceHistoryState final {
        std::size_t undoEntries{};
        std::size_t redoEntries{};
        std::size_t retainedBytes{};
        SourceHistoryBoundary boundary{SourceHistoryBoundary::None};
    };

    /** @brief Revision-fenced semantic byte-range edit; offsets may not split a UTF-8 scalar or BOM. */
    struct SourceTextEdit final {
        std::uint64_t expectedRevision{};
        std::size_t offset{};
        std::size_t eraseBytes{};
        std::string_view insert; /**< Synchronously borrowed, copied before publication; never retained. */
        std::optional<SourceEditingView> beforeView{};
        std::optional<SourceEditingView> afterView{};
    };

    /** @brief One history publication plus originating view metadata.
     * @details Only the matching originating view restores this position. Other views retain and clamp their own
     * selection/scroll against the new text. The service never stores live view state or dispatches to widgets.
     */
    struct SourceHistoryResult final {
        SourceDocumentSnapshot snapshot;
        SourceEditingView view;
    };

    /** @brief Revision-fenced save intent; explicit overwrite consent is tied to exact observed disk bytes. */
    struct SourceSaveRequest final {
        DocumentInstanceId instance;
        std::uint64_t expectedRevision{};
        std::optional<std::string> approvedDiskBytes; /**< Empty optional compares the original disk base. */
    };

    /** @brief Native visibility and durability are distinct outcomes. */
    enum class SourceSaveDisposition : std::uint8_t {
        Durable,
        VisibleDurabilityUnconfirmed
    };

    /** @brief Complete source save receipt; an unconfirmed visible write never claims rollback or a clean base. */
    struct SourceSaveResult final {
        SourceDocumentSnapshot snapshot;
        SourceSaveDisposition disposition{SourceSaveDisposition::Durable};
        std::optional<Error> diagnostic; /**< Preserved filesystem cause for an unconfirmed commit. */
    };

    /** @brief One bounded Save All item; failures remain independently inspectable. */
    struct SourceSaveAllItem final {
        DocumentInstanceId instance;
        Result<SourceSaveResult> result;
    };

    /** @brief Single-owner-thread source sessions; no widget, renderer or ambient activation.
     * @details Open/InspectDisk/Reload are explicit bounded loading operations, never draw-loop work.
     * Edits prepare a complete validated replacement before publishing once. All service calls and moves
     * stay on the creating thread; immutable snapshots alone cross threads. No jobs or persistent locks are owned; saves acquire scoped
     * host filesystem locks. Historical reader leases are caller-owned and must be bounded by the consuming service.
     */
    class SourceDocumentService final {
    public:
        /** @brief Creates a project-contained source owner. @param projectRoot Explicit existing authorized directory.
         * @param limits Finite limits no larger than defaults. @throws std::bad_alloc On owner allocation failure.
         */
        explicit SourceDocumentService(const std::filesystem::path &projectRoot, const SourceDocumentLimits &limits = {});
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
        /** @brief Queries bounded history without I/O. @param instance Exact admitted session.
         * @return Availability or lifecycle/thread failure. */
        [[nodiscard]] Result<SourceHistoryState> History(DocumentInstanceId instance) const;
        /** @brief Undoes one semantic edit without restoring obsolete save or disk observation metadata.
         * @param instance Exact session. @param expectedRevision Current publication. @param cancellation Cooperative observer.
         * @return New monotonic snapshot and originating view; unavailable/stale/cancelled calls preserve history. */
        [[nodiscard]] Result<SourceHistoryResult> Undo(DocumentInstanceId instance, std::uint64_t expectedRevision,
                                                       CancellationToken cancellation = {});
        /** @brief Redoes one undone edit. New edits discard redo; successful reload discards both directions.
         * @param instance Exact session. @param expectedRevision Current publication. @param cancellation Cooperative observer.
         * @return New snapshot and view, or failure preserving text and history. */
        [[nodiscard]] Result<SourceHistoryResult> Redo(DocumentInstanceId instance, std::uint64_t expectedRevision,
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
        /** @brief Saves the exact current revision after disk-base conflict checks and tracked durable publication.
         * @param request Revision and optional exact external-byte overwrite approval.
         * @param files Host-composed filesystem; callbacks may not mutate document/routing ownership.
         * @param cancellation Checked before native replacement; a visible commit is never cancelled retroactively.
         * @return Complete receipt or typed precommit failure preserving current text/base and destination.
         */
        [[nodiscard]] Result<SourceSaveResult> Save(const SourceSaveRequest &request, DurableFileSystem &files,
                                                    CancellationToken cancellation = {});
        /** @brief Advances owner-lane source autosave; admits at most one ordinary conflict-checked save per call.
         * @param elapsedSeconds Finite positive host delta; invalid deltas perform no work.
         * @param delaySeconds Zero disables/reset timers; otherwise clamped to 1..300 seconds after the latest revision.
         * @param files Host-composed durable filesystem, used only at a due save transition.
         * @return No due work, or the exact save result. Failures retain dirty text and retry after at least 30 seconds.
         * @details Uses the same revision fence, exact disk-base checks and publication path as explicit Save.
         * No overwrite consent is synthesized. Timers die with each document session. Runs on the owner lane.
         */
        [[nodiscard]] std::optional<Result<SourceSaveResult>> UpdateAutosave(float elapsedSeconds, int delaySeconds,
                                                                             DurableFileSystem &files);
        /** @brief Saves every dirty admitted document in stable admission order, retaining per-document failures.
         * @param files Host-composed filesystem. @param cancellation Remaining items return cancellation after request.
         * @return Bounded result list or admission/allocation failure before any save starts.
         */
        [[nodiscard]] Result<std::vector<SourceSaveAllItem>> SaveAll(DurableFileSystem &files, CancellationToken cancellation = {});
        /** @brief Releases one owner session; detached snapshots remain alive. @param instance Exact session.
         * @return Success or typed failure. Closing dirty sessions requires the host's explicit discard/save decision.
         */
        [[nodiscard]] Result<void> Close(DocumentInstanceId instance);
        /** @brief Closes admission and releases all owner roots without invalidating detached readers.
         * @return Success or wrong-thread failure; repeated shutdown is idempotent.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        friend class SourceFileOpenService;
        /** @brief Prepares a history replay completely before changing its cursor or publishing. */
        [[nodiscard]] Result<SourceHistoryResult> ReplayHistory(DocumentInstanceId instance, std::uint64_t expectedRevision, bool redo,
                                                                CancellationToken cancellation);
        /** @brief Saves to a host-validated replacement identity without postcommit identity allocation. */
        [[nodiscard]] Result<SourceSaveResult> SaveTo(const SourceSaveRequest &request, DurableFileSystem &files, DocumentIdentity identity,
                                                      std::filesystem::path path, CancellationToken cancellation);
        /** @brief Checks owner-thread lifecycle before accessing service storage. */
        [[nodiscard]] Result<void> CheckAccess() const;
        struct Storage;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Editor
