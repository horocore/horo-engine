#include "Horo/Terrain/TerrainProducerSnapshot.h"

#include <limits>

namespace Horo::Terrain {
    namespace {
        /** @brief Cache replacement stays within one host/cell incarnation and never revives older coordinate generations. */
        bool SameScope(const TerrainProducerSnapshotHeader &old, const TerrainProducerSnapshotHeader &next) noexcept {
            if (old.origin.identity != next.origin.identity || next.origin.revision < old.origin.revision ||
                next.origin.generation < old.origin.generation || old.cell.has_value() != next.cell.has_value())
                return false;
            return !old.cell || (old.cell->partition == next.cell->partition && old.cell->epoch == next.cell->epoch &&
                                 old.cell->cell == next.cell->cell && next.cell->generation >= old.cell->generation);
        }

        /** @brief Requires exact attempt succession and prevents regression of independently versioned semantic roots. */
        bool Successor(const TerrainProducerSnapshotHeader &old, const TerrainProducerSnapshotHeader &next) noexcept {
            return old.terrain == next.terrain && old.world == next.world && old.consumer == next.consumer && SameScope(old, next) &&
                   old.request.Value() != std::numeric_limits<std::uint64_t>::max() && next.request.Value() == old.request.Value() + 1 &&
                   next.revision.content.Value() >= old.revision.content.Value() &&
                   next.revision.residency.Value() >= old.revision.residency.Value() &&
                   next.revision.mutation.Value() >= old.revision.mutation.Value() &&
                   next.revision.capability.Value() >= old.revision.capability.Value();
        }
    }  // namespace

    /** @copydoc TerrainProducerSnapshotOwner::Publish */
    Result<void> TerrainProducerSnapshotOwner::Publish(TerrainProducerSnapshot candidate,
                                                       const std::optional<TerrainProducerSnapshotHeader> expectedCurrent,
                                                       const CancellationToken &cancellation) {
        if (closed_)
            return Result<void>::Failure(MakeError(TerrainProducerErrors::Closed));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(TerrainProducerErrors::Cancelled));
        if (!candidate.IsValid())
            return Result<void>::Failure(MakeError(TerrainProducerErrors::Invalid));
        if (current_) {
            if (!expectedCurrent || current_->Header() != *expectedCurrent || !Successor(current_->Header(), candidate.Header()))
                return Result<void>::Failure(MakeError(TerrainProducerErrors::Stale));
        } else if (expectedCurrent) {
            return Result<void>::Failure(MakeError(TerrainProducerErrors::Stale));
        }
        current_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc TerrainProducerSnapshotOwner::Snapshot */
    Result<TerrainProducerSnapshot> TerrainProducerSnapshotOwner::Snapshot() const {
        if (closed_)
            return Result<TerrainProducerSnapshot>::Failure(MakeError(TerrainProducerErrors::Closed));
        if (!current_)
            return Result<TerrainProducerSnapshot>::Failure(MakeError(TerrainProducerErrors::Unavailable));
        return Result<TerrainProducerSnapshot>::Success(*current_);
    }

    /** @copydoc TerrainProducerSnapshotOwner::Shutdown */
    void TerrainProducerSnapshotOwner::Shutdown() noexcept {
        closed_ = true;
        current_.reset();
    }
}  // namespace Horo::Terrain
