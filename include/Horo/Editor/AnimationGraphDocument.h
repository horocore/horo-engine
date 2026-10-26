#pragma once

/** @file AnimationGraphDocument.h
 * @brief Validated animation graph source, semantic history and revision-fenced save captures.
 */
#include "Horo/Animation/AnimationGraphSource.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <memory>
#include <span>

namespace Horo::Editor {
    namespace AnimationDocumentErrors {
        extern const ErrorCodeDescriptor InvalidCommand;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor HistoryUnavailable;
        extern const ErrorCodeDescriptor HistoryLimit;
        extern const ErrorCodeDescriptor DirtyDocument;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor Exhausted;
        extern const ErrorCodeDescriptor WrongThread;
    }  // namespace AnimationDocumentErrors

    namespace Detail {
        struct AnimationDocumentRoot;
        struct AnimationDocumentOwner;
    }  // namespace Detail

    /** @brief Finite document/history reservations; callers may lower but cannot raise hard bounds. */
    struct AnimationGraphDocumentLimits final {
        Animation::AnimationGraphLimits graph;
        Animation::AnimationGraphSourceLimits source;
        std::size_t historyEntries{64};
        std::size_t historyBytes{8 * 1024 * 1024};
    };

    /** @brief Changes the entry while preserving the asset and skeleton identity. */
    struct SetAnimationGraphEntry final {
        Animation::GraphDefinitionId entry;
    };

    /** @brief Inserts or replaces one stable typed parameter. */
    struct PutAnimationGraphParameter final {
        Animation::GraphParameter value;
    };

    /** @brief Removes one parameter; references must be removed within the same transaction. */
    struct RemoveAnimationGraphParameter final {
        Animation::GraphParameterId id;
    };

    /** @brief Inserts or replaces one complete definition with stable nodes, pins and connections. */
    struct PutAnimationGraphDefinition final {
        Animation::GraphDefinition value;
    };

    /** @brief Removes one definition; callers replace its entry/references within the same transaction. */
    struct RemoveAnimationGraphDefinition final {
        Animation::GraphDefinitionId id;
    };

    /** @brief Closed source operations; final topology is validated before publication. */
    using AnimationGraphDocumentCommand = std::variant<SetAnimationGraphEntry, PutAnimationGraphParameter, RemoveAnimationGraphParameter,
                                                       PutAnimationGraphDefinition, RemoveAnimationGraphDefinition>;

    /** @brief Owned immutable source capture, valid after edits, close and document destruction.
     * Canonical bytes use the existing Animation codec. The capture grants no disk-write authority.
     * Accessors require a capture that has not been moved from. Consumers bound retained capture
     * leases separately from document-owned current/saved roots and semantic history.
     */
    class AnimationGraphDocumentSnapshot final {
    public:
        /** @brief Returns the host-issued Asset document identity. */
        [[nodiscard]] const DocumentIdentity &Identity() const noexcept;
        /** @brief Returns the monotonic mutation revision at capture. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Returns session-local semantic content identity, restored by undo/redo. */
        [[nodiscard]] std::uint64_t State() const noexcept;
        /** @brief Returns validated immutable typed graph data. */
        [[nodiscard]] const Animation::AnimationGraphData &Data() const noexcept;
        /** @brief Returns complete canonical source bytes for Save or recovery; lifetime follows this capture. */
        [[nodiscard]] std::string_view SourceBytes() const noexcept;

    private:
        friend class AnimationGraphDocument;
        AnimationGraphDocumentSnapshot(DocumentIdentity identity, std::uint64_t revision, std::uint64_t state,
                                       std::shared_ptr<const Detail::AnimationDocumentRoot> root,
                                       std::shared_ptr<const Detail::AnimationDocumentOwner> owner);
        DocumentIdentity identity_;
        std::uint64_t revision_{};
        std::uint64_t state_{};
        std::shared_ptr<const Detail::AnimationDocumentRoot> root_;
        std::shared_ptr<const Detail::AnimationDocumentOwner> owner_;
    };

    /** @brief Explicit host decision before replacing or closing dirty source. */
    enum class AnimationDocumentDiscardPolicy : std::uint8_t {
        RequireClean,
        DiscardChanges
    };

    /** @brief Single owner-thread authoring authority; source history is independent from preview playback.
     * All mutations validate detached candidates and reserve semantic history before publication.
     * No runtime dependency lookup, evaluator, widget or source I/O is owned. Save/recovery writers
     * use Snapshot().SourceBytes() through the shared Assets/DocumentRecovery publication services.
     * Only a successful durable source publication may call MarkSaved. Allocation failure propagates
     * as std::bad_alloc and preserves every authored state, revision and history cursor.
     */
    class AnimationGraphDocument final {
    public:
        /** @brief Opens a host-issued Asset document, explicitly migrating supported typed source.
         * @param identity Unique current document instance.
         * @param source Detached source; asset and skeleton identities remain fixed for this session.
         * @param limits Project-lowered codec, schema and history limits.
         * @param cancellation Captured synchronous admission fence.
         * @return Owned document or original validation/cancellation/identity error. Migrated source stays dirty.
         */
        [[nodiscard]] static Result<std::unique_ptr<AnimationGraphDocument>> Open(DocumentIdentity identity,
                                                                                  const Animation::AnimationGraphData &source,
                                                                                  const AnimationGraphDocumentLimits &limits = {},
                                                                                  CancellationToken cancellation = {});
        ~AnimationGraphDocument();
        AnimationGraphDocument(const AnimationGraphDocument &) = delete;
        AnimationGraphDocument &operator=(const AnimationGraphDocument &) = delete;
        /** @brief Copies one immutable source capture. @return Capture or wrong-thread/closed failure. */
        [[nodiscard]] Result<AnimationGraphDocumentSnapshot> Snapshot() const;
        /** @brief Reports unsaved semantic intent. @return Owner-thread query; false when closed. */
        [[nodiscard]] bool IsDirty() const noexcept;
        /** @brief Reports a retained semantic undo step. @return Owner-thread query. */
        [[nodiscard]] bool CanUndo() const noexcept;
        /** @brief Reports a retained semantic redo step. @return Owner-thread query. */
        [[nodiscard]] bool CanRedo() const noexcept;
        /** @brief Commits at most 64 typed operations atomically; no-op edits preserve redo/revision.
         * @param expectedRevision Current capture revision. @param commands Coalesced semantic edits.
         * @param cancellation Synchronous cancellation fence.
         * @return Success or stale/closed/schema/capacity/cancellation error without partial publication.
         */
        [[nodiscard]] Result<void> Execute(std::uint64_t expectedRevision, std::span<const AnimationGraphDocumentCommand> commands,
                                           CancellationToken cancellation = {});
        /** @brief Reverts one complete transaction without changing the saved-state identity.
         * @param expectedRevision Current revision. @param cancellation Synchronous cancellation fence.
         * @return Success or lifecycle/history/schema failure without mutation.
         */
        [[nodiscard]] Result<void> Undo(std::uint64_t expectedRevision, CancellationToken cancellation = {});
        /** @brief Reapplies one complete transaction through canonical validation.
         * @param expectedRevision Current revision. @param cancellation Synchronous cancellation fence.
         * @return Success or lifecycle/history/schema failure without mutation.
         */
        [[nodiscard]] Result<void> Redo(std::uint64_t expectedRevision, CancellationToken cancellation = {});
        /** @brief Acknowledges exact bytes durably saved by the host, including an older capture.
         * @param snapshot Source capture actually published through the shared source writer.
         * @return Success or stale/foreign/closed error. Newer edits remain dirty; reload fences earlier captures.
         */
        [[nodiscard]] Result<void> MarkSaved(const AnimationGraphDocumentSnapshot &snapshot);
        /** @brief Replaces source after the host has accepted external bytes; clears history on success.
         * @param expectedRevision Current revision. @param source Detached replacement with exact asset/skeleton identity.
         * @param policy Explicit permission to discard dirty intent. @param cancellation Synchronous cancellation fence.
         * @return Success or dirty/invalid/stale/schema failure preserving the prior source and history.
         */
        [[nodiscard]] Result<void> Reload(std::uint64_t expectedRevision, const Animation::AnimationGraphData &source,
                                          AnimationDocumentDiscardPolicy policy = AnimationDocumentDiscardPolicy::RequireClean,
                                          CancellationToken cancellation = {});
        /** @brief Applies explicitly accepted recovery bytes as one reversible authored transaction; never marks saved.
         * @param expectedRevision Current revision. @param bytes Existing source-codec payload from the shared recovery owner.
         * @param cancellation Synchronous cancellation fence.
         * @return Success or original parse/history/identity error; recovery cannot overwrite canonical disk source.
         */
        [[nodiscard]] Result<void> ApplyRecovery(std::uint64_t expectedRevision, std::string_view bytes,
                                                 CancellationToken cancellation = {});
        /** @brief Releases source/history after the host has saved, recovered or explicitly discarded intent.
         * @param policy Explicit dirty-source decision. @return Success or dirty/wrong-thread/invalid policy failure.
         */
        [[nodiscard]] Result<void> Close(AnimationDocumentDiscardPolicy policy = AnimationDocumentDiscardPolicy::RequireClean);

    private:
        struct Impl;
        explicit AnimationGraphDocument(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] Result<void> Check(std::uint64_t expectedRevision, CancellationToken cancellation) const;
        [[nodiscard]] Result<void> Commit(const Animation::AnimationGraphData &candidate, CancellationToken cancellation);
        [[nodiscard]] Result<void> Replay(std::uint64_t expectedRevision, bool redo, CancellationToken cancellation);
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
