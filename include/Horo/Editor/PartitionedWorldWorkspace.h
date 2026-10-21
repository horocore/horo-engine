#pragma once

/** @file PartitionedWorldWorkspace.h
 * @brief Editor-owned partitioned-world metadata and independent authoring working set.
 */

#include "Horo/Scene/SceneSourceModel.h"
#include "Horo/WorldStreaming/StreamingDesiredState.h"
#include "Horo/WorldStreaming/StreamingSourceRange.h"
#include "Horo/WorldStreaming/WorldAuthoringContract.h"
#include "Horo/WorldStreaming/WorldSpanningObjectPlan.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace Horo::Editor {
    class SceneDocument;

    /** @brief Stable failures owned by the Editor document boundary. */
    namespace WorldDocumentErrors {
        /** @brief Malformed source metadata, owner identity or resource ceilings. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief Unknown source schema, policy or lifecycle disposition. */
        extern const ErrorCodeDescriptor Unsupported;
        /** @brief Duplicate identities or inconsistent world/page/layer membership. */
        extern const ErrorCodeDescriptor Conflict;
        /** @brief Owner, incarnation, snapshot revision or page successor no longer matches. */
        extern const ErrorCodeDescriptor Stale;
        /** @brief Source metadata or transient working-set capacity is exceeded. */
        extern const ErrorCodeDescriptor Capacity;
        /** @brief No open document or owner admission is cancelling/closed. */
        extern const ErrorCodeDescriptor Lifecycle;
        /** @brief Dirty source metadata cannot be silently discarded. */
        extern const ErrorCodeDescriptor Dirty;
        /** @brief Cancellation was observed before a publication. */
        extern const ErrorCodeDescriptor Cancelled;
        /** @brief Service access was attempted from a non-owning thread. */
        extern const ErrorCodeDescriptor WrongThread;
        /** @brief Workspace generation or publication revision cannot advance without wrapping. */
        extern const ErrorCodeDescriptor Exhausted;
    }  // namespace WorldDocumentErrors

    namespace Detail {
        struct WorldDocumentGenerationTag;
        struct WorldDocumentRevisionTag;
    }  // namespace Detail

    /** @brief Non-wrapping incarnation issued by one workspace owner. */
    using WorldDocumentGeneration = Foundation::Detail::NonZeroId64<Detail::WorldDocumentGenerationTag, WorldDocumentErrors::Invalid>;
    /** @brief Non-wrapping publication sequence, including transient working-set changes. */
    using WorldDocumentRevision = Foundation::Detail::NonZeroId64<Detail::WorldDocumentRevisionTag, WorldDocumentErrors::Invalid>;

    /** @brief Exact workspace, incarnation and publication required by every mutation. */
    struct WorldDocumentFence final {
        Assets::AssetId owner{};
        WorldDocumentGeneration generation{};
        WorldDocumentRevision revision{};
        [[nodiscard]] auto operator<=>(const WorldDocumentFence &) const noexcept = default;
    };

    /** @brief Authored settings; no cooked cells, packages or mounted runtime state are required. */
    struct PartitionedWorldSettings final {
        WorldStreaming::WorldPartitionId partition{};
        WorldStreaming::WorldPartitionBounds bounds{};
        WorldStreaming::WorldCellQuantizationPolicy grid;
        std::uint32_t maximumDirectCellsPerObject{1}; /**< Source spanning-policy threshold; at most 4096. */
    };

    /** @brief One persistent object address and its stable authored layer membership. */
    struct PartitionedWorldObject final {
        WorldStreaming::WorldSpatialObjectDescriptor descriptor{};
        WorldStreaming::StreamingLayerId layer{};
        std::uint8_t assignmentLod{};
        std::optional<WorldStreaming::WorldSpanningObjectDirective> spanning; /**< Exact source override, never widget state. */
        [[nodiscard]] auto operator<=>(const PartitionedWorldObject &) const noexcept = default;
    };

    /** @brief Complete immutable source-page revision, not a cooked streaming cell. */
    struct PartitionedWorldPage final {
        WorldStreaming::WorldAuthoringPageDescriptor head{};
        std::vector<PartitionedWorldObject> objects;
        [[nodiscard]] auto operator<=>(const PartitionedWorldPage &) const noexcept = default;
    };

    /** @brief Authored relevance volume; runtime owner tokens are deliberately absent. */
    struct PartitionedWorldSource final {
        WorldStreaming::StreamingSourceId id;
        WorldStreaming::StreamingSourceRevision revision;
        WorldStreaming::StreamingSourceShape shape;
        WorldStreaming::StreamingSourceIntent intent{WorldStreaming::StreamingSourceIntent::Gameplay};
        WorldStreaming::StreamingDesiredResidency target{WorldStreaming::StreamingDesiredResidency::Loaded};
        WorldStreaming::StreamingSourcePriority priority;
        std::array<float, 3> debugColor{0.2F, 0.7F, 1.0F}; /**< Linear display color, each component in [0,1]. */
        [[nodiscard]] bool operator==(const PartitionedWorldSource &) const = default;
    };

    /** @brief Fixed-capacity control-point result; viewport draw paths allocate no point storage. */
    class WorldSourceGizmoPointSet final {
    public:
        /** @brief Returns the admitted active prefix. @return Immutable shape control points. */
        [[nodiscard]] std::span<const Math::WorldCoordinate64> Points() const noexcept {
            return {points_.data(), count_};
        }

    private:
        friend Result<WorldSourceGizmoPointSet> WorldSourceGizmoPoints(const PartitionedWorldSource &source);
        std::array<Math::WorldCoordinate64, WorldStreaming::StreamingSourceRangeLimits::MaximumPathPointCount + 1> points_{};
        std::size_t count_{};
    };

    /** @brief Projects canonical shape control points without runtime registration or floating-origin conversion.
     * Sphere: center/radius; box: min/max; frustum: anchor/six plane offsets; path: vertices/half-extent.
     * @param source Authored source. @return Exact bounded handle points or typed geometry/arithmetic failure. */
    [[nodiscard]] Result<WorldSourceGizmoPointSet> WorldSourceGizmoPoints(const PartitionedWorldSource &source);
    /** @brief Applies a viewport handle draft; the workspace command remains the sole publication path.
     * @param source Original authored value. @param handle Index from WorldSourceGizmoPoints.
     * @param position Exact millimeter position, borrowed only for this call. @return Complete valid source draft or typed rejection. */
    [[nodiscard]] Result<PartitionedWorldSource> MoveWorldSourceGizmo(const PartitionedWorldSource &source, std::size_t handle,
                                                                      const Math::WorldCoordinate64 &position);

    /** @brief One atomic source insert/replace/remove command against the exact document publication. */
    struct WorldSourceEdit final {
        WorldDocumentFence expected;
        WorldStreaming::StreamingSourceId source;
        std::optional<PartitionedWorldSource> value; /**< Empty removes; present values keep the exact source ID. */
    };

    /** @brief Owned editable source metadata; pages may describe unloaded geometry. */
    struct PartitionedWorldDocument final {
        std::uint16_t version{1}; /**< In-memory model schema; not a persistent file-format version. */
        PartitionedWorldSettings settings;
        std::vector<WorldStreaming::WorldLayerDescriptor> layers;
        std::vector<PartitionedWorldPage> pages;
        std::vector<PartitionedWorldSource> sources; /**< Canonical authored sources, never an independent viewport store. */
    };

    /** @brief Mandatory finite ceilings; requested ceilings cannot exceed these default hard maxima. */
    struct WorldDocumentLimits final {
        std::size_t maximumLayers{256};
        std::size_t maximumPages{4096};
        std::size_t maximumObjects{65536};
        std::size_t maximumLayerNameBytes{65536};
        std::uint32_t maximumOpenPages{64};
        std::size_t maximumSources{1024};
        std::size_t maximumSourceHistory{128};
        std::size_t maximumSelectedObjects{256};
        std::size_t maximumPageSourceBytes{16 * 1024 * 1024};     /**< Canonical source input ceiling per loaded page. */
        std::size_t maximumResidentSourceBytes{64 * 1024 * 1024}; /**< Aggregate admitted source bytes, excluding historical readers. */
    };

    /** @brief Read lease over one actual immutable SceneDocument, never a second editable source authority. */
    class WorldPageContentLease final {
    public:
        /** @brief Returns admitted authored objects. @return Owner-independent view retained by this lease. */
        [[nodiscard]] std::span<const SceneSource::SceneObjectSnapshot> Objects() const noexcept;
        /** @brief Returns admitted prefab placements. @return Owner-independent view retained by this lease. */
        [[nodiscard]] std::span<const SceneSource::ScenePrefabInstance> PrefabInstances() const noexcept;
        /** @brief Queries actual document dirty state. @return Whether content requires explicit save/discard. */
        [[nodiscard]] bool IsDirty() const noexcept;

    private:
        friend class PartitionedWorldWorkspace;
        std::shared_ptr<const SceneDocument> document_;
    };

    /** @brief One loaded page charged to the editor owner, independently of runtime residency. */
    struct WorldLoadedPage final {
        WorldStreaming::WorldAuthoringPageDescriptor head;
        WorldPageContentLease content;
        std::size_t sourceBytes{}; /**< Admission charge; not a measurement of allocator memory. */
        bool pinned{};             /**< Explicit owner-local pin; does not bypass capacity. */
    };

    /** @brief Undoable presentation filter; matching is exact UTF-8 substring, never locale-dependent. */
    struct WorldLayerOutlinerFilter final {
        std::string name;
        std::optional<WorldStreaming::WorldLayerOwnership> ownership;
        [[nodiscard]] auto operator<=>(const WorldLayerOutlinerFilter &) const noexcept = default;
    };

    /** @brief Inserts one explicitly identified layer at a document ordinal; IDs are never row indices. */
    struct CreateWorldLayer final {
        WorldStreaming::WorldLayerDescriptor layer;
        std::size_t position{};
    };

    /** @brief Moves an existing stable layer to its final ordinal in the complete unfiltered document. */
    struct ReorderWorldLayer final {
        WorldStreaming::StreamingLayerId layer;
        std::size_t position{};
    };

    /** @brief Changes the outliner filter without changing authored layers or current authoring context. */
    struct FilterWorldLayers final {
        WorldLayerOutlinerFilter filter;
    };

    /** @brief Selects an existing layer, or explicitly clears authoring context; filtering never retargets it. */
    struct SetCurrentWorldLayer final {
        std::optional<WorldStreaming::StreamingLayerId> layer;
    };

    /** @brief Canonical typed outliner intent, executed by the document/history owner. */
    using WorldLayerCommand = std::variant<CreateWorldLayer, ReorderWorldLayer, FilterWorldLayers, SetCurrentWorldLayer>;

    /** @brief Immutable read lease; retained leases survive replacement, close and workspace destruction. */
    struct PartitionedWorldSnapshot final {
        WorldDocumentFence fence;
        PartitionedWorldDocument document;
        std::vector<Assets::AssetId> openPages; /**< Transient authoring working set, never runtime residency. */
        WorldLayerOutlinerFilter layerFilter;
        std::optional<WorldStreaming::StreamingLayerId> currentLayer;
        std::size_t layerUndoCount{};
        std::size_t layerRedoCount{};
        std::vector<WorldLoadedPage> loadedPages;
        std::vector<WorldStreaming::WorldAuthoringObjectAddress> selectedObjects; /**< Transient selection; never runtime entities. */
        std::optional<WorldStreaming::WorldAuthoringObjectAddress> primary;       /**< Optional selected primary address. */
        bool dirty{}; /**< Authored metadata changed since explicit Open/Replace; never an assertion of disk durability. */
    };

    /** @brief Shared immutable snapshot safe for synchronized handoff to readers. */
    using PartitionedWorldSnapshotLease = std::shared_ptr<const PartitionedWorldSnapshot>;

    /** @brief One coherent world-view capture. The camera/renderer use meters relative to this exact global origin.
     * Host increments viewRevision whenever the origin or active view changes, independently of source document revisions. */
    struct WorldViewportBinding final {
        PartitionedWorldSnapshotLease snapshot;
        Math::WorldCoordinate64 globalOrigin;
        std::uint64_t viewRevision{};
    };

    /** @brief Converts an exact global point into a bounded, millimeter-resolvable viewport position.
     * @param point Canonical source point, borrowed for this call. @param origin Exact view origin, borrowed for this call.
     * @return Local meters, or Unsupported outside the +/-8000 meter local precision window. */
    [[nodiscard]] Result<Math::Vec3> WorldSourceViewportPoint(const Math::WorldCoordinate64 &point, const Math::WorldCoordinate64 &origin);
    /** @brief Adds a bounded local meter position to an exact global origin without converting absolute coordinates to float.
     * @param origin Exact view origin, borrowed for this call. @param local Local meters, quantized to the nearest millimeter.
     * @return Canonical global point or typed invalid/arithmetic rejection. */
    [[nodiscard]] Result<Math::WorldCoordinate64> WorldSourceViewportGlobal(const Math::WorldCoordinate64 &origin, Math::Vec3 local);

    /** @brief Finite query budgets; automatic plus effective cell lists share the aggregate ceiling. */
    struct WorldObjectInspectionLimits final {
        std::uint32_t maximumCellsPerObject{4096};
        std::uint32_t maximumTotalCells{65536};
    };

    /** @brief Complete explanation for one selected exact source object, including unloaded-page metadata.
     * @details A diagnostic preserves the source bounds/override and available automatic coverage; no effective placement is claimed.
     * Always-present objects have NonSpatial placement and empty coverage. Spanning means coverage of more than one cell;
     * requiresDirective means coverage exceeds the authored direct-cell threshold.
     */
    struct WorldObjectInspection final {
        PartitionedWorldObject source;
        std::vector<WorldStreaming::StreamingCellId> automaticCells;
        std::vector<WorldStreaming::StreamingCellId> placementCells;
        bool spanning{};
        bool requiresDirective{};
        std::optional<WorldStreaming::WorldSpanningObjectPlacement> placement;
        std::optional<Error> diagnostic; /**< Original WorldStreaming validation identity; never replaced by display text. */
    };

    /** @brief Immutable selection explanation tied to the exact source AND selection publication. */
    struct WorldObjectSelectionInspection final {
        WorldDocumentFence fence;
        std::optional<WorldStreaming::WorldAuthoringObjectAddress> primary;
        std::vector<WorldObjectInspection> objects; /**< Stable selection order. Empty selection is valid. */
    };

    /** @brief Shared immutable Inspector result; no source, runtime or widget mutation capability. */
    using WorldObjectInspectionLease = std::shared_ptr<const WorldObjectSelectionInspection>;

    /** @brief Explicit dirty-state disposition; Cancel is never equivalent to discard. */
    enum class WorldDocumentDisposition : std::uint8_t {
        Preserve,
        Discard
    };

    /**
     * @brief Single-threaded Editor authority for source metadata and open authoring pages.
     * All calls belong to the constructing thread. Readers use immutable leases only.
     * Inputs are borrowed only for each synchronous call; limits and published metadata are owned copies.
     * Caller storage is copied before publication; no callback, I/O, runtime activation or implicit save occurs.
     * Owner IDs are host-issued and never reused across workspace instances. Allocation exceptions preserve state.
     * Metadata and admitted Scene payloads are bounded tooling state, never frame-hot work. Historical leases are budgeted by consumers.
     * @note Version one fixes spatial pages and revision-checked publication per WorldAuthoringContract; no alternate policy is implied.
     * @throws std::bad_alloc If preparation allocation fails; no publication or generation is consumed.
     */
    class PartitionedWorldWorkspace final {
    public:
        /** @brief Creates an empty active owner. @param owner Host-issued workspace incarnation. @param limits Bounded metadata ceilings.
         */
        explicit PartitionedWorldWorkspace(Assets::AssetId owner, const WorldDocumentLimits &limits = {});
        PartitionedWorldWorkspace(const PartitionedWorldWorkspace &) = delete;
        PartitionedWorldWorkspace &operator=(const PartitionedWorldWorkspace &) = delete;
        PartitionedWorldWorkspace(PartitionedWorldWorkspace &&) = delete;
        PartitionedWorldWorkspace &operator=(PartitionedWorldWorkspace &&) = delete;

        /** @brief Admits an initial complete source document. @param document Source metadata copied on success.
         * @param cancellation Operation cancellation fence. @return Owned immutable snapshot or typed rejection; existing documents are not
         * replaced. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Open(const PartitionedWorldDocument &document,
                                                                 std::stop_token cancellation = {});
        /** @brief Captures current immutable state. @return Lease or typed unavailable/wrong-thread failure. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Snapshot() const;
        /** @brief Commits a complete typed source-metadata transaction without replacing its incarnation.
         * Unchanged pages retain their exact immutable head and objects; changed pages require the next non-wrapping revision.
         * Loaded pages must remain unchanged; use CommitLoadedPage for their metadata/content successor.
         * Removed unloaded pages leave the working set atomically. No mutable storage escapes.
         * @param document Complete proposed metadata, including settings/layers/pages/objects. @param expected Exact current fence.
         * @param cancellation Operation cancellation fence. @return Committed dirty snapshot, unchanged lease for no-op, or typed
         * rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Commit(const PartitionedWorldDocument &document,
                                                                   const WorldDocumentFence &expected, std::stop_token cancellation = {});
        /** @brief Applies one source command and records one bounded undo step. Revisions are owner-issued.
         * @param edit Exact fence, identity and complete desired value, or removal.
         * @param cancellation Cancellation before publication preserves source and history.
         * @return Committed snapshot or typed rejection; no-op commands consume no history or revision. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> EditSource(const WorldSourceEdit &edit, std::stop_token cancellation = {});
        /** @brief Replays one source-history step with fresh monotonic revisions; other metadata remains unchanged.
         * @param expected Exact current document fence, borrowed for this call. @param redo Selects redo instead of undo.
         * @param cancellation Cancellation fence. @return Snapshot or typed unavailable/stale/cancelled rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> ReplaySources(const WorldDocumentFence &expected, bool redo,
                                                                          std::stop_token cancellation = {});
        /** @brief Applies one undoable layer command atomically on the owning thread.
         * Source creates/reorders mark dirty; filter/context commands remain transient. No-ops retain revision/history.
         * @param command Typed semantic intent. @param expected Exact current document fence.
         * @param cancellation Cancellation checked before publication. @return New snapshot or typed atomic rejection.
         */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> ExecuteLayerCommand(const WorldLayerCommand &command,
                                                                                const WorldDocumentFence &expected,
                                                                                std::stop_token cancellation = {});
        /** @brief Creates a named layer with an owner-selected unused stable identity, appended in authored order.
         * @param name Complete UTF-8 layer name. @param expected Exact current fence.
         * @param cancellation Cancellation checked before publication. @return One undoable source command result or typed failure.
         */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> CreateLayer(std::string name, const WorldDocumentFence &expected,
                                                                        std::stop_token cancellation = {});
        /** @brief Replays the next exact semantic layer transaction without changing pages or working-set state.
         * @param redo False for undo, true for redo. @param expected Exact current fence.
         * @param cancellation Cancellation checked before publication. @return New snapshot or typed rejection; empty history is Conflict.
         */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> ReplayLayerCommand(bool redo, const WorldDocumentFence &expected,
                                                                               std::stop_token cancellation = {});
        /** @brief Projects visible stable IDs in authored order from the canonical filter.
         * @return Complete owned rows or typed lifecycle/thread rejection; current layer may be filtered out.
         */
        [[nodiscard]] Result<std::vector<WorldStreaming::StreamingLayerId>> VisibleLayers() const;
        /** @brief Atomically changes metadata admission without touching payloads, source pages or dirty state.
         * Loaded pages cannot be removed here; use UnloadPage after resolving dirty/selection/pin dependencies.
         * @param pages Distinct admitted page assets; order is preserved. @param expected Exact current owner fence.
         * @param cancellation Operation cancellation fence. @return New immutable snapshot, unchanged lease for a no-op, or typed
         * rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetWorkingSet(std::span<const Assets::AssetId> pages,
                                                                          const WorldDocumentFence &expected,
                                                                          std::stop_token cancellation = {});
        /** @brief Loads exact canonical Scene source into an actual document after bounded validation, outside draw paths.
         * The host authorizes/reads source bytes; this operation performs no file or runtime I/O. Unknown components are rejected.
         * @param page Exact current authored page head. @param source Canonical source, copied before publication.
         * @param expected Current owner fence. @param cancellation Cancellation fence.
         * @return New lease or typed rejection preserving every existing payload, selection, pin and dirty value. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> LoadPage(const WorldStreaming::WorldAuthoringPageDescriptor &page,
                                                                     std::string_view source, const WorldDocumentFence &expected,
                                                                     std::stop_token cancellation = {});
        /** @brief Publishes edited page metadata and actual Scene content together; creates a dirty immutable successor.
         * Selected objects cannot disappear; clear their selection explicitly first. Metadata-only Commit cannot change loaded pages.
         * @param page Exact successor metadata. @param source Canonical complete edited source.
         * @param expected Current owner fence. @param cancellation Cancellation fence.
         * @return Atomic successor or typed rejection; old leases remain valid. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> CommitLoadedPage(const PartitionedWorldPage &page, std::string_view source,
                                                                             const WorldDocumentFence &expected,
                                                                             std::stop_token cancellation = {});
        /** @brief Releases one clean, unselected, unpinned payload and its open-page slot, retaining source metadata.
         * Dirty payloads require document-level explicit discard or a future durable save, never silent eviction.
         * @param page Admitted page asset. @param expected Current fence. @param cancellation Cancellation fence.
         * @return Atomic release or typed dirty/conflict/lifecycle rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> UnloadPage(Assets::AssetId page, const WorldDocumentFence &expected,
                                                                       std::stop_token cancellation = {});
        /** @brief Changes an explicit editor pin on a loaded page without changing content or runtime demands.
         * @param page Loaded page asset. @param pinned Requested pin. @param expected Current fence.
         * @param cancellation Cancellation fence. @return Successor, unchanged lease for no-op, or typed rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetPagePinned(Assets::AssetId page, bool pinned,
                                                                          const WorldDocumentFence &expected,
                                                                          std::stop_token cancellation = {});
        /** @brief Atomically selects distinct source addresses; opening geometry is not required.
         * @param objects Existing page-scoped addresses in presentation order. @param primary Optional address within that selection.
         * @param expected Exact source/selection fence, borrowed only for this call.
         * @param cancellation Checked before publication. @return Immutable selection root or typed zero-mutation rejection.
         * @details Loaded addresses also require membership in their actual payload; unload retains selected dependencies.
         * No-op retains its revision. Explicit unloaded source removal prunes selection in the same commit. Replace clears it.
         */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetSelection(
            std::span<const WorldStreaming::WorldAuthoringObjectAddress> objects,
            const std::optional<WorldStreaming::WorldAuthoringObjectAddress> &primary, const WorldDocumentFence &expected,
            std::stop_token cancellation = {});
        /** @brief Explains the current selected objects through canonical WorldStreaming assignment/spanning kernels.
         * @param expected Exact source/selection publication. @param limits Bounded owned output ceilings, no larger than defaults.
         * @param cancellation Cooperative observer. @return Complete immutable explanation or typed rejection; never truncates.
         * @details Explicit owner-thread tooling query, never draw/frame-hot work. Invalid bounds or conflicting geometric overrides
         * become per-object diagnostics; stale requests, unsupported policy, resource exhaustion and cancellation reject the whole query.
         * Retained results survive source replacement, selection changes, close and shutdown. No I/O, runtime activation or mutation.
         */
        [[nodiscard]] Result<WorldObjectInspectionLease> InspectSelectedObjects(const WorldDocumentFence &expected,
                                                                                WorldObjectInspectionLimits limits = {},
                                                                                std::stop_token cancellation = {}) const;

        /** @brief Replaces a document explicitly, issuing a fresh generation and an empty working set.
         * @param document Complete new source metadata. @param expected Exact current fence.
         * @param disposition Explicit permission to discard dirty metadata. @param cancellation Operation cancellation fence.
         * @return New clean admission snapshot or typed failure preserving the old document and working set. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Replace(
            const PartitionedWorldDocument &document, const WorldDocumentFence &expected,
            WorldDocumentDisposition disposition = WorldDocumentDisposition::Preserve, std::stop_token cancellation = {});
        /** @brief Closes the document, preserving retained leases; a subsequent Open gets a new generation.
         * @param expected Exact current fence. @param disposition Explicit dirty-state decision. @return Success or typed rejection. */
        [[nodiscard]] Result<void> Close(const WorldDocumentFence &expected,
                                         WorldDocumentDisposition disposition = WorldDocumentDisposition::Preserve);
        /** @brief Gates further admission/edits while preserving queries and explicit teardown. @return Success or wrong-thread failure. */
        [[nodiscard]] Result<void> BeginCancellation();
        /** @brief Permanently shuts down this owner without callbacks, implicit save or runtime eviction.
         * @param disposition Explicit dirty-state decision. @return Success or typed rejection; failed teardown retains state. */
        [[nodiscard]] Result<void> Shutdown(WorldDocumentDisposition disposition = WorldDocumentDisposition::Preserve);

    private:
        /** @brief Prepares and publishes an admitted fresh incarnation with strong failure isolation. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Install(const PartitionedWorldDocument &document, std::stop_token cancellation);
        /** @brief Checks mutation thread, lifecycle and exact incarnation/revision. */
        [[nodiscard]] Result<void> CheckFence(const WorldDocumentFence &expected) const;
        /** @brief Publishes one fully prepared successor after a final cancellation check. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Publish(std::shared_ptr<PartitionedWorldSnapshot> candidate,
                                                                    std::stop_token cancellation);

        /** @brief History contains authored source values only; generic metadata commits invalidate it. */
        struct SourceHistoryStep {
            std::vector<PartitionedWorldSource> before;
            std::vector<PartitionedWorldSource> after;
        };

        /** @brief Invalidates source history after external metadata publication or lifecycle change. */
        void ClearSourceHistory() noexcept;
        std::vector<SourceHistoryStep> sourceHistory_;
        std::size_t sourceHistoryCursor_{};
        std::uint64_t sourceRevision_{};

        /** @brief Exact reversible layer-section values; no pages, runtime data or presentation widgets are retained. */
        struct LayerState final {
            std::vector<WorldStreaming::WorldLayerDescriptor> layers;
            WorldLayerOutlinerFilter filter;
            std::optional<WorldStreaming::StreamingLayerId> current;
        };

        /** @brief One semantic transaction charged against finite entry and byte budgets. */
        struct LayerTransaction final {
            LayerState before;
            LayerState after;
            std::size_t bytes{};
            bool beforeDirty{};
            bool afterDirty{};
        };

        /** @brief Shares complete document validation with ordinary metadata transactions. */
        [[nodiscard]] Result<void> ValidateLayerCandidate(const PartitionedWorldDocument &document) const;
        /** @brief Validates and applies a typed intent to detached candidate storage. */
        [[nodiscard]] Result<void> ApplyLayerCommand(const WorldLayerCommand &command, PartitionedWorldSnapshot &candidate) const;
        /** @brief Resets layer history after complete external metadata commits or lifecycle replacement. */
        void ClearLayerHistory() noexcept;
        std::vector<LayerTransaction> layerHistory_;
        std::size_t layerCursor_{};
        /** @brief Prepares a real SceneDocument payload and checks exact metadata/content membership and finite admission charges. */
        [[nodiscard]] Result<WorldLoadedPage> PreparePage(const PartitionedWorldPage &page, std::string_view source, bool dirty,
                                                          std::stop_token cancellation) const;
        /** @brief Checks replacement-aware page/object/source-byte charges before decoding or copying payloads. */
        [[nodiscard]] Result<void> CheckPageCapacity(Assets::AssetId page, std::size_t objects, std::size_t bytes) const;
        /** @brief Prepares coherent loaded-page metadata with successor and dependent-selection validation. */
        [[nodiscard]] Result<PartitionedWorldDocument> PreparePageEdit(const PartitionedWorldPage &page) const;
        Assets::AssetId owner_;
        WorldDocumentLimits limits_;
        const std::thread::id thread_{std::this_thread::get_id()};
        WorldStreaming::WorldAuthoringOwnerState state_{WorldStreaming::WorldAuthoringOwnerState::Active};
        std::uint64_t generation_{};
        PartitionedWorldSnapshotLease snapshot_;
    };
}  // namespace Horo::Editor
