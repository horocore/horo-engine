#pragma once

/** @file PartitionedWorldWorkspace.h
 * @brief Editor-owned partitioned-world metadata and independent authoring working set.
 */

#include "Horo/WorldStreaming/StreamingDesiredState.h"
#include "Horo/WorldStreaming/StreamingSourceRange.h"
#include "Horo/WorldStreaming/WorldAuthoringContract.h"
#include "Horo/WorldStreaming/WorldSpatialObjectDescriptor.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace Horo::Editor {
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
    };

    /** @brief One persistent object address and its stable authored layer membership. */
    struct PartitionedWorldObject final {
        WorldStreaming::WorldSpatialObjectDescriptor descriptor{};
        WorldStreaming::StreamingLayerId layer{};
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

    /** @brief Projects canonical shape control points without runtime registration or floating-origin conversion.
     * Sphere: center/radius; box: min/max; frustum: anchor/six plane offsets; path: vertices/half-extent.
     * @param source Authored source. @return Exact bounded handle points or typed geometry/arithmetic failure. */
    [[nodiscard]] Result<std::vector<Math::WorldCoordinate64>> WorldSourceGizmoPoints(const PartitionedWorldSource &source);
    /** @brief Applies a viewport handle draft; the workspace command remains the sole publication path.
     * @param source Original authored value. @param handle Index from WorldSourceGizmoPoints.
     * @param position Exact millimeter position. @return Complete valid source draft or typed rejection. */
    [[nodiscard]] Result<PartitionedWorldSource> MoveWorldSourceGizmo(const PartitionedWorldSource &source, std::size_t handle,
                                                                      Math::WorldCoordinate64 position);

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
    };

    /** @brief Immutable read lease; retained leases survive replacement, close and workspace destruction. */
    struct PartitionedWorldSnapshot final {
        WorldDocumentFence fence;
        PartitionedWorldDocument document;
        std::vector<Assets::AssetId> openPages; /**< Transient authoring working set, never runtime residency. */
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
     * @param point Canonical source point. @param origin Exact origin captured by the active world view.
     * @return Local meters, or Unsupported outside the +/-8000 meter local precision window. */
    [[nodiscard]] Result<Math::Vec3> WorldSourceViewportPoint(Math::WorldCoordinate64 point, Math::WorldCoordinate64 origin);
    /** @brief Adds a bounded local meter position to an exact global origin without converting absolute coordinates to float.
     * @param origin Exact view origin. @param local Local meters, quantized to the nearest millimeter.
     * @return Canonical global point or typed invalid/arithmetic rejection. */
    [[nodiscard]] Result<Math::WorldCoordinate64> WorldSourceViewportGlobal(Math::WorldCoordinate64 origin, Math::Vec3 local);

    /** @brief Explicit dirty-state disposition; Cancel is never equivalent to discard. */
    enum class WorldDocumentDisposition : std::uint8_t {
        Preserve,
        Discard
    };

    /**
     * @brief Single-threaded Editor authority for source metadata and open authoring pages.
     * All calls belong to the constructing thread. Readers use immutable leases only.
     * Caller storage is copied before publication; no callback, I/O, runtime activation or implicit save occurs.
     * Owner IDs are host-issued and never reused across workspace instances. Allocation exceptions preserve state.
     * This is bounded tooling metadata, not a frame-hot payload owner. Retained historical leases are budgeted by consumers.
     * @note Version one fixes spatial pages and revision-checked publication per WorldAuthoringContract; no alternate policy is implied.
     * @throws std::bad_alloc If preparation allocation fails; no publication or generation is consumed.
     */
    class PartitionedWorldWorkspace final {
    public:
        /** @brief Creates an empty active owner. @param owner Host-issued workspace incarnation. @param limits Bounded metadata ceilings.
         */
        explicit PartitionedWorldWorkspace(Assets::AssetId owner, WorldDocumentLimits limits = {});
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
         * Removed pages are removed from the working set atomically. No mutable storage escapes.
         * @param document Complete proposed metadata, including settings/layers/pages/objects. @param expected Exact current fence.
         * @param cancellation Operation cancellation fence. @return Committed dirty snapshot, unchanged lease for no-op, or typed
         * rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Commit(const PartitionedWorldDocument &document, WorldDocumentFence expected,
                                                                   std::stop_token cancellation = {});
        /** @brief Applies one source command and records one bounded undo step. Revisions are owner-issued.
         * @param edit Exact fence, identity and complete desired value, or removal.
         * @param cancellation Cancellation before publication preserves source and history.
         * @return Committed snapshot or typed rejection; no-op commands consume no history or revision. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> EditSource(const WorldSourceEdit &edit, std::stop_token cancellation = {});
        /** @brief Replays one source-history step with fresh monotonic revisions; other metadata remains unchanged.
         * @param expected Exact current document fence. @param redo Selects redo instead of undo.
         * @param cancellation Cancellation fence. @return Snapshot or typed unavailable/stale/cancelled rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> ReplaySources(WorldDocumentFence expected, bool redo,
                                                                          std::stop_token cancellation = {});
        /** @brief Atomically changes the transient set without touching source pages or dirty state.
         * @param pages Distinct admitted page assets; order is preserved. @param expected Exact current owner fence.
         * @param cancellation Operation cancellation fence. @return New immutable snapshot, unchanged lease for a no-op, or typed
         * rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetWorkingSet(std::span<const Assets::AssetId> pages,
                                                                          WorldDocumentFence expected, std::stop_token cancellation = {});
        /** @brief Replaces a document explicitly, issuing a fresh generation and an empty working set.
         * @param document Complete new source metadata. @param expected Exact current fence.
         * @param disposition Explicit permission to discard dirty metadata. @param cancellation Operation cancellation fence.
         * @return New clean admission snapshot or typed failure preserving the old document and working set. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Replace(
            const PartitionedWorldDocument &document, WorldDocumentFence expected,
            WorldDocumentDisposition disposition = WorldDocumentDisposition::Preserve, std::stop_token cancellation = {});
        /** @brief Closes the document, preserving retained leases; a subsequent Open gets a new generation.
         * @param expected Exact current fence. @param disposition Explicit dirty-state decision. @return Success or typed rejection. */
        [[nodiscard]] Result<void> Close(WorldDocumentFence expected,
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
        [[nodiscard]] Result<void> CheckFence(WorldDocumentFence expected) const;
        /** @brief Publishes one fully prepared successor after a final cancellation check. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Publish(std::shared_ptr<PartitionedWorldSnapshot> candidate,
                                                                    std::stop_token cancellation);

        /** @brief History contains authored source values only; generic metadata commits invalidate it. */
        struct SourceHistoryStep {
            std::vector<PartitionedWorldSource> before;
            std::vector<PartitionedWorldSource> after;
        };

        std::vector<SourceHistoryStep> sourceHistory_;
        std::size_t sourceHistoryCursor_{};
        std::uint64_t sourceRevision_{};
        Assets::AssetId owner_;
        WorldDocumentLimits limits_;
        const std::thread::id thread_{std::this_thread::get_id()};
        WorldStreaming::WorldAuthoringOwnerState state_{WorldStreaming::WorldAuthoringOwnerState::Active};
        std::uint64_t generation_{};
        PartitionedWorldSnapshotLease snapshot_;
    };
}  // namespace Horo::Editor
