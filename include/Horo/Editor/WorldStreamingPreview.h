#pragma once

/** @file WorldStreamingPreview.h
 * @brief Bounded immutable viewport projection of canonical streaming intent, residency and reasons.
 */

#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "Horo/WorldStreaming/WorldStreamingDiagnosticSnapshot.h"

#include <utility>

namespace Horo::Editor {
    /** @brief Exact source, runtime, planner and viewport publications joined by a preview. */
    struct WorldStreamingPreviewFence final {
        WorldDocumentFence document;
        WorldStreaming::WorldPartitionRegistryBinding registry;
        WorldStreaming::WorldStreamingDiagnosticRevision diagnostics;
        std::uint64_t planRevision{};
        std::uint64_t viewRevision{}; /**< Host viewport publication; zero is invalid. */
        [[nodiscard]] auto operator<=>(const WorldStreamingPreviewFence &) const noexcept = default;
    };

    /** @brief Mandatory finite allocation/work ceilings for a tooling projection, never a streaming policy. */
    struct WorldStreamingPreviewLimits final {
        std::uint32_t cells{4096};
        std::uint32_t attempts{1024};
        std::uint32_t decisions{4096};
        std::uint32_t contributions{16384};
    };

    /** @brief One canonical cell with distinct desired state and actual generation-scoped runtime evidence. */
    struct WorldStreamingPreviewCell final {
        WorldStreaming::WorldPartitionCellHandle cell;
        WorldStreaming::WorldPartitionBounds bounds; /**< Exact inclusive registry bounds; not an authoring-page footprint. */
        std::optional<WorldStreaming::StreamingPlannedCell> desired; /**< Empty means absent from this exact plan, never implicit Loaded. */
        std::vector<WorldStreaming::StreamingPlannedSourceCell>
            contributions;                                              /**< Canonical source demand, including predicted provenance. */
        std::vector<WorldStreaming::StreamingCellStateRecord> attempts; /**< Includes independently retiring generations. */
        std::vector<WorldStreaming::StreamingDiagnosticFailureRecord> failures; /**< Typed causes survive disabled event instrumentation. */
        std::vector<WorldStreaming::StreamingDiagnosticDecisionEvent> decisions; /**< Authority-authored reasons in event sequence order. */
    };

    /**
     * @brief Immutable read-only joined preview with no planner, camera, document or runtime authority.
     * @details Build only when input revisions change, outside draw paths. The host captures one safe-point set and supplies its
     * exact expected fence. Retained values survive replacement/shutdown; hosts must compare Fence before rendering. No source
     * registration, activation, policy edits, I/O or callbacks occur. Allocation failure throws without publishing a partial value.
     */
    class WorldStreamingPreview final {
    public:
        /**
         * @brief Validates and copies a complete bounded projection without retaining borrowed planner data.
         * @param source Existing controller-owned immutable document lease.
         * @param registry Exact mounted index snapshot owning cell identity and spatial bounds.
         * @param plan Canonical planner publication borrowed only for this call.
         * @param diagnostics Canonical authority diagnostics borrowed only for this call.
         * @param expected Exact host-captured publications; mismatches reject the entire projection.
         * @param limits Allocation and work ceilings bounded by the implementation maxima.
         * @param cancellation Cooperative cancellation checked before, during and after preparation.
         * @return Complete immutable value or typed invalid, stale, conflict, lifecycle, capacity or cancellation failure.
         * @throws std::bad_alloc If bounded allocation fails; no partial projection or authority mutation occurs.
         */
        [[nodiscard]] static Result<WorldStreamingPreview> Create(
            PartitionedWorldSnapshotLease source, const WorldStreaming::WorldPartitionRegistrySnapshot &registry,
            const WorldStreaming::StreamingSourcePlanView &plan, const WorldStreaming::WorldStreamingDiagnosticSnapshot &diagnostics,
            WorldStreamingPreviewFence expected, WorldStreamingPreviewLimits limits = {}, std::stop_token cancellation = {});

        /** @brief Returns exact captured publications. @return Immutable source/runtime/view fence. */
        [[nodiscard]] const WorldStreamingPreviewFence &Fence() const noexcept {
            return fence_;
        }

        /** @brief Returns canonically ordered cell rows. @return Immutable owned rows valid for this value's lifetime. */
        [[nodiscard]] std::span<const WorldStreamingPreviewCell> Cells() const noexcept {
            return cells_;
        }

        /** @brief Returns the controller-issued source lease. @return Immutable lease retained across document replacement. */
        [[nodiscard]] const PartitionedWorldSnapshotLease &Source() const noexcept {
            return source_;
        }

        /** @brief Returns captured runtime lifecycle. @return Actual authority lifecycle; cancellation is never inferred as unload. */
        [[nodiscard]] WorldStreaming::WorldStreamingRuntimeCompositionState Lifecycle() const noexcept {
            return lifecycle_;
        }

    private:
        /** @brief Owns a completely validated immutable projection. */
        WorldStreamingPreview(PartitionedWorldSnapshotLease source, WorldStreamingPreviewFence fence,
                              WorldStreaming::WorldStreamingRuntimeCompositionState lifecycle, std::vector<WorldStreamingPreviewCell> cells)
            : source_(std::move(source)), fence_(fence), lifecycle_(lifecycle), cells_(std::move(cells)) {}

        PartitionedWorldSnapshotLease source_;
        WorldStreamingPreviewFence fence_;
        WorldStreaming::WorldStreamingRuntimeCompositionState lifecycle_;
        std::vector<WorldStreamingPreviewCell> cells_;
    };

    /** @brief One immutable local cell box; index addresses the matching preview row. */
    struct WorldStreamingPreviewBox final {
        std::size_t cellIndex{};
        std::array<Math::Vec3, 8> corners;
    };

    /** @brief Bounded immutable geometry prepared outside the viewport draw path. */
    struct WorldStreamingPreviewGeometry final {
        std::shared_ptr<const WorldStreamingPreview> preview;
        Math::WorldCoordinate64 globalOrigin;
        std::uint64_t viewRevision{};
        std::vector<WorldStreamingPreviewBox> boxes;
        std::size_t outsideLocalWindow{}; /**< Explicitly culled cells, never a residency fact. */
    };

    /**
     * @brief Projects exact bounds through the canonical active-world origin conversion without allocating in draw paths.
     * @param preview Complete owned evidence projection.
     * @param view Canonical controller-issued active view, including the same immutable document lease.
     * @param maximumBoxes Positive immutable geometry ceiling up to 256; overflow rejects the entire result.
     * @param cancellation Cooperative preparation cancellation.
     * @return Complete geometry or typed stale, invalid, capacity or cancellation error. Out-of-window cells are counted explicitly.
     */
    [[nodiscard]] Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> BuildWorldStreamingPreviewGeometry(
        std::shared_ptr<const WorldStreamingPreview> preview, const WorldViewportBinding &view, std::size_t maximumBoxes = 256,
        std::stop_token cancellation = {});

    /**
     * @brief Application-composed read-only bridge to the existing mounted streaming owners; never another streaming owner.
     * @details All methods run on the constructing thread after the native streaming safe point and before editor presentation.
     * The native mount calls BindRuntime with its actual registry, planner and current diagnostic value; all three outlive the binding
     * and may only be replaced on this lane. It calls UnbindRuntime before destruction/unmount. Refresh reads Snapshot/Plan directly;
     * it never registers sources, services I/O, changes policies, advances the planner or fabricates residency. No bound mount is an
     * explicit Lifecycle result. Host composition registers this one bridge and lends it to workspace controllers.
     */
    class WorldStreamingPreviewHost final {
    public:
        /** @brief Creates an unbound bridge on the native/editor owner lane; no runtime state is synthesized. */
        WorldStreamingPreviewHost() = default;
        WorldStreamingPreviewHost(const WorldStreamingPreviewHost &) = delete;
        WorldStreamingPreviewHost &operator=(const WorldStreamingPreviewHost &) = delete;
        /**
         * @brief Binds actual native owners after checking one coherent publication; failed replacement preserves the old binding.
         * @param registry Existing mounted registry, borrowed until explicit unbind.
         * @param planner Existing canonical planner, borrowed until explicit unbind.
         * @param diagnostics Existing authority-owned diagnostic value; its address remains stable while its publication advances.
         * @return Success or typed owner/publication/lifecycle rejection. Different mounted owners require explicit unbind first.
         */
        [[nodiscard]] Result<void> BindRuntime(const WorldStreaming::WorldPartitionRegistry &registry,
                                               const WorldStreaming::StreamingSourcePlanner &planner,
                                               const WorldStreaming::WorldStreamingDiagnosticSnapshot &diagnostics);
        /** @brief Revokes borrows and cached presentation before unmount. @param owner Exact bound owner.
         * @return Success or wrong-thread/stale rejection; an old unmount cannot revoke a newer owner. */
        [[nodiscard]] Result<void> UnbindRuntime(WorldStreaming::StreamingRuntimeOwnerToken owner);
        /** @brief Captures actual owners and prepares geometry only when input publications change.
         * @param view Canonical active-world view. @return Cached immutable geometry or typed unavailable/stale/capacity result.
         * Failure clears the displayed cache; no previous snapshot is passed off as current. */
        [[nodiscard]] Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> Refresh(const WorldViewportBinding &view);
        /** @brief Drops only retained presentation when a workspace closes; native owner binding remains explicit. */
        void ClearPresentation() noexcept;

    private:
        /** @brief Joins a validated native capture with the canonical view and enforces monotonic publication history. */
        [[nodiscard]] Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> Project(
            const WorldViewportBinding &view, const WorldStreaming::WorldPartitionRegistrySnapshot &registry,
            const WorldStreaming::StreamingSourcePlanView &plan);
        /** @brief Revokes stale presentation while preserving the native anti-rollback publication floor. */
        [[nodiscard]] Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> CaptureFailure(const Error &error);
        const std::thread::id thread_{std::this_thread::get_id()};
        const WorldStreaming::WorldPartitionRegistry *registry_{};
        const WorldStreaming::StreamingSourcePlanner *planner_{};
        const WorldStreaming::WorldStreamingDiagnosticSnapshot *diagnostics_{};
        WorldStreaming::StreamingRuntimeOwnerToken owner_{};
        std::shared_ptr<const WorldStreamingPreviewGeometry> cached_;
        std::optional<WorldStreamingPreviewFence> latest_;
        std::optional<std::pair<std::uint64_t, Math::WorldCoordinate64>> latestView_;
    };
}  // namespace Horo::Editor
