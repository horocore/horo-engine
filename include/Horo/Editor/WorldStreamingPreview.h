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
}  // namespace Horo::Editor
