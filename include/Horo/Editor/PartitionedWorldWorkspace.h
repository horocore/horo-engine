#pragma once

/** @file PartitionedWorldWorkspace.h
 * @brief Editor-owned partitioned-world metadata and independent authoring working set.
 */

#include "Horo/Scene/SceneSourceModel.h"
#include "Horo/WorldStreaming/WorldAuthoringContract.h"
#include "Horo/WorldStreaming/WorldSpatialObjectDescriptor.h"

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
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

    /** @brief Owned editable source metadata; pages may describe unloaded geometry. */
    struct PartitionedWorldDocument final {
        std::uint16_t version{1}; /**< In-memory model schema; not a persistent file-format version. */
        PartitionedWorldSettings settings;
        std::vector<WorldStreaming::WorldLayerDescriptor> layers;
        std::vector<PartitionedWorldPage> pages;
    };

    /** @brief Mandatory finite ceilings; requested ceilings cannot exceed these default hard maxima. */
    struct WorldDocumentLimits final {
        std::size_t maximumLayers{256};
        std::size_t maximumPages{4096};
        std::size_t maximumObjects{65536};
        std::size_t maximumLayerNameBytes{65536};
        std::uint32_t maximumOpenPages{64};
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

    /** @brief Immutable read lease; retained leases survive replacement, close and workspace destruction. */
    struct PartitionedWorldSnapshot final {
        WorldDocumentFence fence;
        PartitionedWorldDocument document;
        std::vector<Assets::AssetId> openPages; /**< Transient authoring working set, never runtime residency. */
        std::vector<WorldLoadedPage> loadedPages;
        std::vector<WorldStreaming::WorldAuthoringObjectAddress> selectedObjects;
        std::optional<WorldStreaming::WorldAuthoringObjectAddress> primarySelection;
        bool dirty{}; /**< Authored metadata changed since explicit Open/Replace; never an assertion of disk durability. */
    };

    /** @brief Shared immutable snapshot safe for synchronized handoff to readers. */
    using PartitionedWorldSnapshotLease = std::shared_ptr<const PartitionedWorldSnapshot>;

    /** @brief Explicit dirty-state disposition; Cancel is never equivalent to discard. */
    enum class WorldDocumentDisposition : std::uint8_t {
        Preserve,
        Discard
    };

    /**
     * @brief Single-threaded Editor authority for source metadata and open authoring pages.
     * All calls belong to the constructing thread. Readers use immutable leases only.
     * Caller storage is copied before publication; no callback, file I/O, runtime activation or implicit save occurs.
     * Owner IDs are host-issued and never reused across workspace instances. Allocation exceptions preserve state.
     * Metadata and admitted Scene payloads are bounded tooling state, never frame-hot work. Historical leases are budgeted by consumers.
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
         * Loaded pages must remain unchanged; use CommitLoadedPage for their metadata/content successor.
         * Removed unloaded pages leave the working set atomically. No mutable storage escapes.
         * @param document Complete proposed metadata, including settings/layers/pages/objects. @param expected Exact current fence.
         * @param cancellation Operation cancellation fence. @return Committed dirty snapshot, unchanged lease for no-op, or typed
         * rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> Commit(const PartitionedWorldDocument &document, WorldDocumentFence expected,
                                                                   std::stop_token cancellation = {});
        /** @brief Atomically changes metadata admission without touching payloads, source pages or dirty state.
         * Loaded pages cannot be removed here; use UnloadPage after resolving dirty/selection/pin dependencies.
         * @param pages Distinct admitted page assets; order is preserved. @param expected Exact current owner fence.
         * @param cancellation Operation cancellation fence. @return New immutable snapshot, unchanged lease for a no-op, or typed
         * rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetWorkingSet(std::span<const Assets::AssetId> pages,
                                                                          WorldDocumentFence expected, std::stop_token cancellation = {});
        /** @brief Loads exact canonical Scene source into an actual document after bounded validation, outside draw paths.
         * The host authorizes/reads source bytes; this operation performs no file or runtime I/O. Unknown components are rejected.
         * @param page Exact current authored page head. @param source Canonical source, copied before publication.
         * @param expected Current owner fence. @param cancellation Cancellation fence.
         * @return New lease or typed rejection preserving every existing payload, selection, pin and dirty value. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> LoadPage(WorldStreaming::WorldAuthoringPageDescriptor page,
                                                                     std::string_view source, WorldDocumentFence expected,
                                                                     std::stop_token cancellation = {});
        /** @brief Publishes edited page metadata and actual Scene content together; creates a dirty immutable successor.
         * Selected objects cannot disappear; clear their selection explicitly first. Metadata-only Commit cannot change loaded pages.
         * @param page Exact successor metadata. @param source Canonical complete edited source.
         * @param expected Current owner fence. @param cancellation Cancellation fence.
         * @return Atomic successor or typed rejection; old leases remain valid. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> CommitLoadedPage(const PartitionedWorldPage &page, std::string_view source,
                                                                             WorldDocumentFence expected,
                                                                             std::stop_token cancellation = {});
        /** @brief Releases one clean, unselected, unpinned payload and its open-page slot, retaining source metadata.
         * Dirty payloads require document-level explicit discard or a future durable save, never silent eviction.
         * @param page Admitted page asset. @param expected Current fence. @param cancellation Cancellation fence.
         * @return Atomic release or typed dirty/conflict/lifecycle rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> UnloadPage(Assets::AssetId page, WorldDocumentFence expected,
                                                                       std::stop_token cancellation = {});
        /** @brief Changes an explicit editor pin on a loaded page without changing content or runtime demands.
         * @param page Loaded page asset. @param pinned Requested pin. @param expected Current fence.
         * @param cancellation Cancellation fence. @return Successor, unchanged lease for no-op, or typed rejection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetPagePinned(Assets::AssetId page, bool pinned, WorldDocumentFence expected,
                                                                          std::stop_token cancellation = {});
        /** @brief Replaces page-aware selection atomically; selected pages are unload dependencies, not runtime pins.
         * @param objects Distinct existing loaded object addresses, in presentation order.
         * @param primary Optional member of objects. @param expected Current fence. @param cancellation Cancellation fence.
         * @return Successor, unchanged no-op lease, or typed rejection without losing dependent selection. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetSelection(
            std::span<const WorldStreaming::WorldAuthoringObjectAddress> objects,
            std::optional<WorldStreaming::WorldAuthoringObjectAddress> primary, WorldDocumentFence expected,
            std::stop_token cancellation = {});

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
