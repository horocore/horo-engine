#pragma once
/** @file PCGGraphDocumentService.h @brief Workspace-owned graph sessions, source publication and shared recovery lifecycle. */
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Editor/PCGGraphDocument.h"

namespace Horo::Editor {
    /** @brief Read-only recovery candidate; accepting it requires a separate exact live command fence. */
    struct PCGRecoveryCandidate final {
        DocumentRecoveryRecord record;
        std::shared_ptr<const PCG::PCGGraphAsset> source;
    };

    /** @brief Bounded workspace service independent from panels and the graph canvas.
     * @details Owner-thread only. Host supplies a never-reused incarnation and catalog; no backend discovery.
     * One writable session per AssetId and persistent route. All edits use PCGGraphDocument's one executor.
     * Synchronous bounded save/recovery are explicit control-path operations; there are no workers to detach.
     */
    class PCGGraphDocumentService final {
    public:
        /** @brief Creates bounded owner state with explicit host incarnation.
         * @param root Canonical project root. @param owner Host-generated unique incarnation.
         * @param catalog Owned exact support projection. @param mutations Borrowed shared mutation owner, optional for read-only opening.
         * @param files Borrowed tracked durable owner, optional for read-only opening.
         * @param migrator Optional host-owned deterministic legacy converter; must outlive this service.
         * @pre Borrowed mutation and durable owners outlive this service. The migrator performs no publication or reentrant owner calls.
         */
        PCGGraphDocumentService(std::filesystem::path root, Assets::AssetId owner, PCGDocumentCatalog catalog,
                                const ProjectMutationCoordinator *mutations = nullptr, DurableFileSystem *files = nullptr,
                                const PCG::IPCGGraphSourceMigrator *migrator = nullptr);
        ~PCGGraphDocumentService();
        PCGGraphDocumentService(const PCGGraphDocumentService &) = delete;
        PCGGraphDocumentService &operator=(const PCGGraphDocumentService &) = delete;
        /** @brief Opens or focuses without rereading an existing writable session.
         * @param asset Stable registry identity. @param identity Workspace route. @param path Canonical source.
         * @param migrator Explicit compatible-schema hook. @return Immutable root or zero-admission failure.
         */
        [[nodiscard]] Result<PCGDocumentSnapshot> Open(Assets::AssetId asset, DocumentIdentity identity, const std::filesystem::path &path,
                                                       const PCG::IPCGGraphSourceMigrator *migrator = nullptr);
        /** @brief Queries without source I/O. @param instance Exact route. @return Immutable snapshot or rejection. */
        [[nodiscard]] Result<PCGDocumentSnapshot> Snapshot(DocumentInstanceId instance) const;
        /** @brief Captures all live documents for shared workspace leave/save decisions.
         * @return Finite immutable root list, or lifecycle/thread error. */
        [[nodiscard]] Result<std::vector<PCGDocumentSnapshot>> Snapshots() const;
        /** @brief Executes one semantic transaction. @param instance Exact route. @param command Typed exact fence.
         * @return Success or zero-mutation rejection.
         */
        [[nodiscard]] Result<void> Execute(DocumentInstanceId instance, const PCGDocumentCommand &command);
        /** @brief Replays exact semantic history. @param instance Exact route. @param command Exact header only.
         * @param redo Direction. @return Success or zero-mutation rejection.
         */
        [[nodiscard]] Result<void> Replay(DocumentInstanceId instance, const PCGDocumentCommand &command, bool redo);
        /** @brief Saves through actual Assets/shared project/native publication owners.
         * @param instance Exact route. @param command Exact fence and capability. @return Tracked durable/reconciliation result.
         */
        [[nodiscard]] Result<Assets::AssetSourcePublication> Save(DocumentInstanceId instance, const PCGDocumentCommand &command);
        /** @brief Inspects external source bytes, setting conflict without replacing root/history.
         * @param instance Exact route. @return Current observation or typed error.
         */
        [[nodiscard]] Result<PCGDocumentSnapshot> InspectExternal(DocumentInstanceId instance);
        /** @brief Explicitly reloads against exact live command revision.
         * @param instance Exact route. @param command Exact fence. @param discardDirty Explicit loss decision.
         * @param migrator Explicit migration hook. @return Success or zero-root-mutation failure.
         */
        [[nodiscard]] Result<void> Reload(DocumentInstanceId instance, const PCGDocumentCommand &command, bool discardDirty,
                                          const PCG::IPCGGraphSourceMigrator *migrator = nullptr);
        /** @brief Captures dirty recovery without changing canonical source or saved state.
         * @param instance Exact route. @param command Exact fence. @return Durable recovery or error.
         */
        [[nodiscard]] Result<void> Autosave(DocumentInstanceId instance, const PCGDocumentCommand &command);
        /** @brief Returns a read-only candidate through the sole PCG codec.
         * @param instance Exact route. @param migrator Explicit migration hook. @return Candidate/absence or bounded error.
         */
        [[nodiscard]] Result<std::optional<PCGRecoveryCandidate>> InspectRecovery(
            DocumentInstanceId instance, const PCG::IPCGGraphSourceMigrator *migrator = nullptr) const;
        /** @brief Explicitly accepts a candidate after recapturing exact durable currentness.
         * @param instance Exact route. @param command Live revision/session/catalog fence. @param candidate Read-only candidate.
         * @return Bounded reversible source transaction or zero-mutation failure; never publishes source.
         */
        [[nodiscard]] Result<void> AcceptRecovery(DocumentInstanceId instance, const PCGDocumentCommand &command,
                                                  const PCGRecoveryCandidate &candidate);
        /** @brief Closes only after shared save/discard/cancel policy; panel detachment must not call this.
         * @param instance Exact route. @param discardDirty Explicit loss decision. @return Success or typed dirty/busy error.
         */
        [[nodiscard]] Result<void> Close(DocumentInstanceId instance, bool discardDirty = false);
        /** @brief Writes all dirty recovery at a lifecycle checkpoint, coalescing identical state.
         * @return Success or first actionable error; every admitted document is attempted.
         */
        [[nodiscard]] Result<void> FlushRecovery();
        /** @brief Closes admission after explicitly preserving recovery or discarding dirty candidates.
         * @param preserveRecovery True writes dirty recovery; false requires explicit discard permission.
         * @param discardDirty Explicit loss decision. @return Success or error with ownership retained for retry.
         */
        [[nodiscard]] Result<void> Shutdown(bool preserveRecovery, bool discardDirty = false);
        /** @brief Replaces exact catalog context for all open roots atomically.
         * @param catalog Strictly newer host-composed projection. @return Success or zero-owner-mutation failure.
         */
        [[nodiscard]] Result<void> ReplaceCatalog(PCGDocumentCatalog catalog);

    private:
        struct State;
        [[nodiscard]] Result<void> CheckAccess() const;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
