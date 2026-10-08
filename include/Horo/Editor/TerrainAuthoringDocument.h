#pragma once

/** @file TerrainAuthoringDocument.h
 * @brief Owner-thread bounded terrain source transactions and exact semantic history.
 */

#include "Horo/Terrain/TerrainSourceImport.h"

#include <map>
#include <optional>
#include <vector>

namespace Horo::Editor {
    namespace TerrainEditErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor WrongDocument;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor CapabilityUnavailable;
        extern const ErrorCodeDescriptor LimitExceeded;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor Exhausted;
        extern const ErrorCodeDescriptor HistoryEmpty;
    }  // namespace TerrainEditErrors
    struct TerrainDocumentSessionTag;
    struct TerrainDocumentStateTag;
    struct TerrainEditOperationTag;
    /** @brief Host-issued session identity; replacement must issue a fresh value. */
    using TerrainDocumentSessionId = Foundation::Detail::NonZeroId64<TerrainDocumentSessionTag, TerrainEditErrors::Invalid>;
    /** @brief Immutable content state identity independent of monotonic publication revision. */
    using TerrainDocumentStateId = Foundation::Detail::NonZeroId64<TerrainDocumentStateTag, TerrainEditErrors::Invalid>;
    /** @brief Host-issued operation correlation identity, with no implicit global allocator. */
    using TerrainEditOperationId = Foundation::Detail::NonZeroId64<TerrainEditOperationTag, TerrainEditErrors::Invalid>;
    /** @brief Document publications share the existing source-revision domain. */
    using TerrainDocumentRevision = Terrain::TerrainSourceRevision;

    /** @brief Explicit source mutation permission; read-only sessions cannot replay history. */
    enum class TerrainAuthoringCapability : std::uint8_t {
        ReadOnly,
        Edit
    };

    /** @brief Canonical increasing-X/Z source rectangle; dimensions are nonzero and half-open. */
    struct TerrainPatchRect final {
        std::uint32_t x{}, z{}, width{}, height{};
        [[nodiscard]] constexpr auto operator<=>(const TerrainPatchRect &) const noexcept = default;
    };

    /** @brief One exact semantic raster result, not a brush algorithm or mutable storage reference.
     * Empty channel vectors mean unchanged. Weight payloads contain all authored layers per pixel.
     */
    struct TerrainRasterPatch final {
        TerrainPatchRect rect;
        std::vector<float> heightsMeters;
        std::vector<std::uint16_t> weights;
        std::vector<std::uint8_t> holes;
    };

    /** @brief Authored placement value; cooked cluster/runtime identity is deliberately absent. */
    struct TerrainAuthoredPlacement final {
        Terrain::FoliageInstanceId id;
        Terrain::FoliageTypeId type;
        std::uint32_t sampleX{}, sampleZ{};
        float offsetMeters{}, scale{1.0F}, yawRadians{};
        [[nodiscard]] bool operator==(const TerrainAuthoredPlacement &) const noexcept = default;
    };

    /** @brief Exact placement assign/remove; an absent value erases an existing stable ID. */
    struct TerrainPlacementEdit final {
        Terrain::FoliageInstanceId id;
        std::optional<TerrainAuthoredPlacement> value;
    };

    /** @brief Finite per-operation and retained-history ceilings, admitted before mutation.
     * Tooling-only storage work; no allocation or history manipulation occurs in draw paths.
     */
    struct TerrainEditLimits final {
        std::size_t maximumPatches{256}, maximumSamples{1'048'576}, maximumPlacementEdits{4'096};
        std::size_t maximumTransactionBytes{32 * 1024 * 1024}, maximumHistoryBytes{64 * 1024 * 1024};
        std::size_t maximumHistoryItems{128}, maximumPlacements{65'536}, maximumDirtyTiles{4'096};
    };

    /** @brief Exact admission fence shared by execute, undo and redo. */
    struct TerrainEditFence final {
        TerrainDocumentSessionId session;
        Terrain::TerrainDatasetId dataset;
        TerrainDocumentRevision expectedRevision;
        Terrain::TerrainCapabilityRevision capabilityRevision;
    };

    /** @brief One atomic edit from any host/tool adapter; overlapping channel writes are rejected. */
    struct TerrainEditOperation final {
        TerrainEditFence fence;
        TerrainEditOperationId operation;
        std::vector<TerrainRasterPatch> patches;
        std::vector<TerrainPlacementEdit> placements;
    };

    /** @brief Exact before/after values for one affected placement; no algorithm replay. */
    struct TerrainPlacementSnapshot final {
        Terrain::FoliageInstanceId id;
        std::optional<TerrainAuthoredPlacement> before, after;
    };

    /** @brief Derived inputs invalidated by a committed canonical source transaction. */
    struct TerrainDependencyInvalidation final {
        bool visual{}, collision{}, navigation{}, foliage{};
    };

    /** @brief Bounded atomic change receipt; seam neighbours and one-sample aprons are included. */
    struct TerrainEditChange final {
        TerrainEditOperationId operation;
        TerrainDocumentRevision revision;
        TerrainDocumentStateId state;
        std::vector<Terrain::TerrainTileId> dirtyTiles;
        TerrainDependencyInvalidation invalidation;
        bool changed{}, dirty{};
    };

    /** @brief Lossless region-only history, owned by the document and never retaining external leases. */
    struct TerrainEditRecord final {
        TerrainEditOperationId operation;
        TerrainDocumentRevision baseRevision, committedRevision;
        TerrainDocumentStateId beforeState, afterState;
        std::vector<TerrainRasterPatch> before, after;
        std::vector<TerrainPlacementSnapshot> placements;
        std::vector<Terrain::TerrainTileId> dirtyTiles;
        TerrainDependencyInvalidation invalidation;
        std::size_t bytes{};
    };

    /** @brief Sole owner of canonical height/weight/hole/placement mutation and semantic undo.
     * All methods execute on one host owner thread. No worker, callback or runtime ownership is
     * retained. Execute stages only affected regions; every fallible step precedes publication.
     * Replacement closes the old object and opens a fresh host-issued session. Borrowed query
     * views expire on any edit/history attempt (including failure), close/move or destruction. Save/cook remains external.
     */
    class TerrainAuthoringDocument final {
    public:
        /** @brief Admits an existing canonical source without importing or copying the dataset.
         * @param session Fresh host-issued document session.
         * @param source Detached canonical source from Assets/import, transferred on success.
         * @param tileCells Number of cells per derived tile; source samples have one canonical owner.
         * @param capability Explicit permission; no fallback is selected.
         * @param limits Finite transaction/history ceilings.
         * @param placements Detached dataset-local placement source loaded by the source owner; never cooked instances.
         * @return Validated owner or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<TerrainAuthoringDocument> Open(TerrainDocumentSessionId session, Terrain::TerrainCanonicalSource source,
                                                                   std::uint32_t tileCells, TerrainAuthoringCapability capability,
                                                                   const TerrainEditLimits &limits = {},
                                                                   const std::vector<TerrainAuthoredPlacement> &placements = {});
        /** @brief Transfers sole ownership and closes donor admission.
         * @param other Owner being retired by transfer.
         */
        TerrainAuthoringDocument(TerrainAuthoringDocument &&other) noexcept;
        /** @brief Replaces this owner and retires donor admission; self-transfer preserves the owner.
         * @param other Owner being retired by transfer.
         * @return This owner after transfer.
         */
        TerrainAuthoringDocument &operator=(TerrainAuthoringDocument &&other) noexcept;
        TerrainAuthoringDocument(const TerrainAuthoringDocument &) = delete;
        TerrainAuthoringDocument &operator=(const TerrainAuthoringDocument &) = delete;
        ~TerrainAuthoringDocument() = default;

        /** @brief Returns immutable canonical storage, never a writable tool interface. @return Owner-bound view. */
        [[nodiscard]] const Terrain::TerrainCanonicalSource &Source() const noexcept {
            return source_;
        }

        /** @brief Returns canonical authored placement storage. @return Owner-bound immutable map. */
        [[nodiscard]] const std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> &Placements() const noexcept {
            return placements_;
        }

        /** @brief Captures the current admission fence. @return Exact session/source/capability revisions. */
        [[nodiscard]] TerrainEditFence Fence() const noexcept;

        /** @brief Returns saved-state comparison independent of history depth. @return Whether source is unsaved. */
        [[nodiscard]] bool IsDirty() const noexcept {
            return state_ != savedState_;
        }

        /** @brief Returns lifecycle admission. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept {
            return closed_;
        }

        /** @brief Returns immutable current content identity. @return State restored by undo/redo. */
        [[nodiscard]] TerrainDocumentStateId State() const noexcept {
            return state_;
        }

        /** @brief Returns retained history size. @return Undo item count. */
        [[nodiscard]] std::size_t UndoCount() const noexcept {
            return undo_.size();
        }

        /** @brief Returns retained redo size. @return Redo item count. */
        [[nodiscard]] std::size_t RedoCount() const noexcept {
            return redo_.size();
        }

        /** @brief Returns exact charged region/metadata history bytes. @return Aggregate undo plus redo charge. */
        [[nodiscard]] std::size_t HistoryBytes() const noexcept {
            return historyBytes_;
        }

        /** @brief Queries the last undo record without transferring mutation authority. @return Borrowed record or null. */
        [[nodiscard]] const TerrainEditRecord *LastUndo() const noexcept {
            return undo_.empty() ? nullptr : &undo_.back();
        }

        /** @brief Validates and commits one bounded semantic transaction, with no partial history on failure.
         * @param operation Exact source results and captured fence; payload is not retained on failure.
         * @param cancellation Cooperative cancellation checked before staging and immediately before commit.
         * @return Atomic change receipt or typed failure; exact no-ops add no revision/history.
         */
        [[nodiscard]] Result<TerrainEditChange> Execute(const TerrainEditOperation &operation, const CancellationToken &cancellation = {});
        /** @brief Atomically restores exact prior values without rerunning tool algorithms.
         * @param fence Exact admission fence.
         * @param cancellation Cancellation before commit preserves source and both stacks.
         * @return One new monotonic revision or typed error.
         */
        [[nodiscard]] Result<TerrainEditChange> Undo(const TerrainEditFence &fence, const CancellationToken &cancellation = {});
        /** @brief Atomically restores the exact committed result.
         * @param fence Exact admission fence.
         * @param cancellation Cancellation before commit preserves source and both stacks.
         * @return One new monotonic revision or typed error.
         */
        [[nodiscard]] Result<TerrainEditChange> Redo(const TerrainEditFence &fence, const CancellationToken &cancellation = {});
        /** @brief Accepts a successfully durably saved immutable content state, even after later edits.
         * @param session Exact publishing session.
         * @param state State identity captured by the source-save owner; cook/preview must never call this.
         * @return Success or wrong-session/closed/invalid-state failure.
         */
        [[nodiscard]] Result<void> AcceptSavedState(TerrainDocumentSessionId session, TerrainDocumentStateId state);

        /** @brief Stops all further mutation/save admission while retaining canonical values for retirement. */
        void Close() noexcept {
            closed_ = true;
        }

    private:
        TerrainAuthoringDocument() = default;
        /** @brief Validates owner-thread lifecycle and exact identity/revision/permission before staging.
         * @param fence Captured source admission fence.
         * @param cancellation Captured cooperative cancellation state.
         * @return Success or a typed admission error without mutation.
         */
        [[nodiscard]] Result<void> Admit(const TerrainEditFence &fence, const CancellationToken &cancellation) const;
        /** @brief Stages placement/history allocations then replays one exact committed record.
         * @param fence Captured source admission fence.
         * @param cancellation Cancellation before publication.
         * @param redo Selects the exact after values rather than before values.
         * @return New revision receipt or typed failure without partial mutation.
         */
        [[nodiscard]] Result<TerrainEditChange> Replay(const TerrainEditFence &fence, const CancellationToken &cancellation, bool redo);
        /** @brief Copies admitted exact primitive values at allocation-free publication.
         * @param patches Validated canonical regions whose dimensions and payloads match the source.
         */
        void Apply(const std::vector<TerrainRasterPatch> &patches) noexcept;
        /** @brief Reserves the receipt/history slot then publishes one allocation-free transaction.
         * @param record Complete detached exact semantic history record.
         * @param staged Preallocated replacement placement nodes.
         * @param cancellation Cancellation checked after history reservation and immediately before publication.
         * @return Atomic committed receipt or cancellation without mutation.
         * @throws std::bad_alloc Before publication only; Execute translates allocation failure.
         */
        [[nodiscard]] Result<TerrainEditChange> Publish(TerrainEditRecord record,
                                                        std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> staged,
                                                        const CancellationToken &cancellation);
        TerrainDocumentSessionId session_;
        Terrain::TerrainCanonicalSource source_;
        std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> placements_;
        TerrainAuthoringCapability capability_{TerrainAuthoringCapability::ReadOnly};
        TerrainEditLimits limits_;
        std::uint32_t tileCells_{};
        TerrainDocumentStateId state_, savedState_;
        std::uint64_t nextState_{2};
        std::vector<TerrainEditRecord> undo_, redo_;
        std::size_t historyBytes_{};
        bool closed_{};
    };
}  // namespace Horo::Editor
