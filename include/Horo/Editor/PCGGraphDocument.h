#pragma once
/** @file PCGGraphDocument.h
 * @brief Single typed PCG source authority using shared semantic history and Assets persistence.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/SemanticDocumentHistory.h"
#include "Horo/PCG/PCGGraphAsset.h"
#include "Horo/PCG/PCGNodeCatalog.h"

namespace Horo::Editor {
    /** @brief Closed zero-mutation command/lifecycle rejection vocabulary. */
    enum class PCGDocumentCommandFailureReason : std::uint8_t {
        Invalid,
        Stale,
        Unauthorized,
        Limit,
        HistoryLimit,
        Closed,
        WrongThread,
        Cancelled,
        Dirty,
        Conflict,
        Busy,
        Exhausted
    };
    /** @brief Returns the stable descriptor for one rejection. @param reason Closed cause. @return Descriptor. */
    [[nodiscard]] const ErrorCodeDescriptor &PCGDocumentError(PCGDocumentCommandFailureReason reason) noexcept;

    /** @brief Never-reused host incarnation and monotonic document opening generation. */
    struct DocumentSessionId final {
        Assets::AssetId owner;
        std::uint64_t generation{};

        [[nodiscard]] bool IsValid() const noexcept {
            return owner.IsValid() && generation != 0;
        }

        auto operator<=>(const DocumentSessionId &) const = default;
    };

    /** @brief Immutable host-composed catalog projection retained by the document, never borrowed. */
    struct PCGDocumentCatalog final {
        std::uint64_t revision{};
        PCG::PCGOperationalTier tier{PCG::PCGOperationalTier::Baseline};
        PCG::PCGGraphSourceLimits limits;
        std::vector<PCG::PCGNodeTypeSupport> types;
    };

    /** @brief Complete source policy edit; no derived plan or preview values. */
    struct SetPCGGraphPolicy final {
        PCG::PCGGenerationMode mode{PCG::PCGGenerationMode::Offline};
        std::uint64_t seed{};
    };

    /** @brief Inserts/replaces one exact semantic node including its stable pin declarations. */
    struct PutPCGNode final {
        PCG::PCGGraphNode value;
    };

    /** @brief Removes one node; callers remove its references in the same atomic transaction. */
    struct RemovePCGNode final {
        PCG::NodeId id;
    };

    /** @brief Inserts/replaces one exact directed edge. */
    struct PutPCGEdge final {
        PCG::PCGGraphEdge value;
    };

    /** @brief Removes one stable directed edge. */
    struct RemovePCGEdge final {
        PCG::EdgeId id;
    };

    /** @brief Inserts/replaces one typed exposed-input binding. */
    struct PutPCGInput final {
        PCG::PCGExposedInput value;
    };

    /** @brief Removes one stable exposed-input binding. */
    struct RemovePCGInput final {
        PCG::ExposedInputId id;
    };

    /** @brief Closed semantic operations supported by source schema 1.1; output semantics live in typed nodes/pins. */
    using PCGDocumentOperation =
        std::variant<SetPCGGraphPolicy, PutPCGNode, RemovePCGNode, PutPCGEdge, RemovePCGEdge, PutPCGInput, RemovePCGInput>;

    /** @brief Exact command fence plus bounded coalesced transaction; cancelled gestures submit nothing. */
    struct PCGDocumentCommand final {
        DocumentSessionId session;
        std::uint64_t expectedRevision{};
        std::uint64_t catalogRevision{};
        bool canEdit{};
        std::vector<PCGDocumentOperation> operations;
        CancellationToken cancellation;
    };

    /** @brief Read-only immutable graph capture; validation/cook/bake cannot acknowledge a source save. */
    struct PCGDocumentSnapshot final {
        Assets::AssetId asset;
        DocumentIdentity identity;
        DocumentSessionId session;
        std::uint64_t revision{};
        std::uint64_t catalogRevision{};
        Sha256Digest state;
        Assets::AssetSourceIdentity acceptedSource;
        PCG::GraphGeneration acceptedGeneration;
        std::shared_ptr<const PCG::PCGGraphAsset> source;
        bool dirty{};
        bool externalConflict{};
    };

    /** @brief Transient typed property draft, never a source or history authority. */
    struct PCGNodePropertyDraft final {
        PCG::NodeId node;
        std::optional<PCG::PCGNodeAuthoringSettings> settings;
        std::optional<Error> diagnostic; /**< Unknown/malformed nodes stay visible and unchanged. */
    };

    /** @brief One explicit inspector gesture captured against exact document and catalog lifetimes.
     * @details Apply emits a single coalesced command; cancel destroys the draft. Reload, history,
     * catalog replacement and close invalidate application through the existing document fences.
     * Presentation fields may contain invalid intermediate values; no authored state changes until Execute succeeds. */
    struct PCGGraphAuthoringDraft final {
        PCGDocumentSnapshot baseline;
        std::uint64_t executableCatalogGeneration{};
        PCG::PCGGraphSourceData values;
        std::vector<PCGNodePropertyDraft> properties;

        /** @brief Captures owned typed controls without repairing unknown settings.
         * @param snapshot Exact source root. @param catalog Host-composed executable schema snapshot.
         * @return Detached draft or invalid capture failure. */
        [[nodiscard]] static Result<PCGGraphAuthoringDraft> Capture(const PCGDocumentSnapshot &snapshot,
                                                                    const PCG::PCGNodeCatalogSnapshot &catalog);
        /** @brief Checks all document, source and schema fences.
         * @param snapshot Current owner snapshot. @param catalog Current executable schemas.
         * @return True only while this gesture can still be applied. */
        [[nodiscard]] bool Matches(const PCGDocumentSnapshot &snapshot, const PCG::PCGNodeCatalogSnapshot &catalog) const noexcept;
        /** @brief Validates schema, capabilities, tier and values before emitting one atomic command.
         * @param current Current owner root. @param catalog Current executable schemas.
         * @param canEdit Explicit writable capability. @return One command or zero-mutation rejection.
         * @details Unsupported modes are preserved in the draft and rejected, never downgraded.
         * Unknown node payloads round-trip unchanged; unrelated supported edits remain possible. */
        [[nodiscard]] Result<PCGDocumentCommand> Command(const PCGDocumentSnapshot &current, const PCG::PCGNodeCatalogSnapshot &catalog,
                                                         bool canEdit) const;
    };

    /** @brief Private exact save capture, shared by source publication and recovery storage. */
    class PCGDocumentSave final {
    public:
        /** @brief Returns captured immutable authored state. @return Exact source/session fence. */
        [[nodiscard]] const PCGDocumentSnapshot &Snapshot() const noexcept {
            return snapshot_;
        }

        /** @brief Returns the single PCG codec's prepared source bytes. @return Immutable owned bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

    private:
        friend class PCGGraphDocument;
        PCGDocumentSnapshot snapshot_;
        std::vector<std::byte> bytes_;
        PCG::GraphGeneration publishedGeneration_;
        std::uint64_t replacementEpoch_{};
    };

    /** @brief Owner-thread graph specialization; detached roots survive close and replacement.
     * @details Host supplies one never-reused session, one registry AssetId, and immutable catalog.
     * Shared SemanticDocumentHistory retains exact semantic element deltas. Assets owns bytes and
     * durable publication; shared ProjectMutation remains held through the entire native operation.
     * No jobs, compiler, evaluator, widget, target or callbacks are owned. Allocations happen before
     * visible root swaps; bad_alloc propagates without committing authored/history state.
     */
    class PCGGraphDocument final {
    public:
        /** @brief Opens one bounded source through the canonical codec; migration remains dirty.
         * @param asset Stable registry asset. @param identity Workspace routing identity.
         * @param session Fresh host-issued lifetime. @param capture Exact durable bytes and identity.
         * @param catalog Owned semantic support. @param history Finite reversible retention budgets.
         * @param migrator Explicit older-schema migration hook, borrowed only for opening.
         * @return Document or typed zero-admission failure; missing nodes remain inert and round-trip losslessly.
         */
        [[nodiscard]] static Result<PCGGraphDocument> Open(Assets::AssetId asset, DocumentIdentity identity, DocumentSessionId session,
                                                           const Assets::AssetSourceCapture &capture, PCGDocumentCatalog catalog,
                                                           SemanticHistoryLimits history = {},
                                                           const PCG::IPCGGraphSourceMigrator *migrator = nullptr);
        ~PCGGraphDocument();
        PCGGraphDocument(PCGGraphDocument &&) noexcept;
        PCGGraphDocument &operator=(PCGGraphDocument &&) noexcept;
        PCGGraphDocument(const PCGGraphDocument &) = delete;
        PCGGraphDocument &operator=(const PCGGraphDocument &) = delete;
        /** @brief Captures the current immutable root. @return Snapshot or closed/thread error. */
        [[nodiscard]] Result<PCGDocumentSnapshot> Snapshot() const;
        /** @brief Executes one complete transaction against exact session/catalog/source revision.
         * @param command Typed finite intent and admission fence. @return Success or zero-mutation failure.
         */
        [[nodiscard]] Result<void> Execute(const PCGDocumentCommand &command);
        /** @brief Restores exact semantic values without compiler/evaluator work.
         * @param fence Exact command header with no operations. @param redo Replay direction.
         * @return Success or zero-mutation history/admission failure.
         */
        [[nodiscard]] Result<void> Replay(const PCGDocumentCommand &fence, bool redo);
        /** @brief Captures validated codec bytes and exact save fence without marking clean.
         * @param fence Exact command header, permission and cancellation. @return Save ticket or rejection.
         */
        [[nodiscard]] Result<PCGDocumentSave> CaptureSave(const PCGDocumentCommand &fence) const;
        /** @brief Publishes captured source through shared project/native exclusive authority.
         * @param save Exact private capture. @param root Canonical project root. @param path Canonical source.
         * @param mutations Shared project coordinator. @param files Tracked native durable implementation.
         * @param cancellation Cooperative observer. @return Durable or reconciliation receipt; failure never marks clean.
         * @details Edits during save remain dirty; close/reload/catalog replacement fence late receipts.
         */
        [[nodiscard]] Result<Assets::AssetSourcePublication> Save(const PCGDocumentSave &save, const std::filesystem::path &root,
                                                                  const std::filesystem::path &path,
                                                                  const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                                  CancellationToken cancellation = {});
        /** @brief Observes incoming source identity without replacing authored state/history.
         * @param identity Exact independently captured source identity. @return Success or lifecycle rejection.
         */
        [[nodiscard]] Result<void> ObserveExternal(const Assets::AssetSourceIdentity &identity);
        /** @brief Explicitly replaces a clean or authorized-discard root and invalidates old save tickets/history.
         * @param fence Exact admission fence. @param capture Complete incoming source. @param discardDirty Explicit loss decision.
         * @param migrator Explicit migration hook. @return Success or zero-mutation failure.
         */
        [[nodiscard]] Result<void> Reload(const PCGDocumentCommand &fence, const Assets::AssetSourceCapture &capture, bool discardDirty,
                                          const PCG::IPCGGraphSourceMigrator *migrator = nullptr);
        /** @brief Admits a read-only recovered source only via an explicit semantic transaction.
         * @param fence Exact live admission fence. @param recovered Canonical read-only candidate.
         * @param expectedBase Exact recovery accepted durable base; different current source rejects.
         * @return Success with bounded reversible history, or zero-mutation failure; never writes source.
         */
        [[nodiscard]] Result<void> AcceptRecovery(const PCGDocumentCommand &fence, const PCG::PCGGraphAsset &recovered,
                                                  const Assets::AssetSourceIdentity &expectedBase);
        /** @brief Replaces catalog support without losing unknown payloads; invalidates pending captures.
         * @param catalog Strictly newer owned support projection. @return Success or zero-mutation failure.
         */
        [[nodiscard]] Result<void> ReplaceCatalog(PCGDocumentCatalog catalog);
        /** @brief Resolves shared save/discard/cancel close policy. @param discardDirty Explicit loss decision.
         * @return Success or dirty/busy/thread failure. Detached readers remain valid.
         */
        [[nodiscard]] Result<void> Close(bool discardDirty = false);
        /** @brief Reports finite semantic history usage. @return Retained undo count. */
        [[nodiscard]] std::size_t UndoCount() const noexcept;
        /** @brief Reports finite semantic history usage. @return Retained redo count. */
        [[nodiscard]] std::size_t RedoCount() const noexcept;

    private:
        friend class PCGGraphDocumentService;

        /** @brief Validates catalog and tier limits through the canonical model before owner publication. */
        [[nodiscard]] static Result<void> ValidateCatalog(const PCGDocumentCatalog &catalog);

        struct CatalogReplacement final {
            PCGDocumentCatalog catalog;
            std::shared_ptr<const PCG::PCGGraphAsset> source;
        };

        [[nodiscard]] Result<CatalogReplacement> PrepareCatalog(PCGDocumentCatalog catalog) const;
        void CommitCatalog(CatalogReplacement &replacement) noexcept;
        struct State;
        explicit PCGGraphDocument(std::unique_ptr<State> state) noexcept;
        [[nodiscard]] Result<void> CheckAccess() const;
        [[nodiscard]] Result<void> Admit(const PCGDocumentCommand &command) const;
        [[nodiscard]] Result<void> Commit(PCG::PCGGraphSourceData candidate, CancellationToken cancellation);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
