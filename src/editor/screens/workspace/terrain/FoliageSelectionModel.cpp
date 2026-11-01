#include "FoliageSelectionModel.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Compare every document mutation admission field, without interpreting selection as ownership. */
        bool Matches(const TerrainEditFence &a, const TerrainEditFence &b) {
            return a.session == b.session && a.dataset == b.dataset && a.expectedRevision == b.expectedRevision &&
                   a.capabilityRevision == b.capabilityRevision;
        }
    }  // namespace

    /** @copydoc FoliageSelectionModel::Replace */
    Result<void> FoliageSelectionModel::Replace(const TerrainAuthoringDocument &document, const TerrainEditFence &fence,
                                                std::span<const Terrain::FoliageInstanceId> ids) {
        if (document.IsClosed())
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        if (!Matches(document.Fence(), fence))
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        if (ids.size() > 65'536)
            return Result<void>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        std::vector<Terrain::FoliageInstanceId> candidate(ids.begin(), ids.end());
        std::ranges::sort(candidate);
        candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
        for (auto id : candidate)
            if (!document.Placements().contains(id))
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
        ids_ = std::move(candidate);
        session_ = fence.session;
        dataset_ = fence.dataset;
        return Result<void>::Success();
    }

    /** @copydoc FoliageSelectionModel::Observe */
    void FoliageSelectionModel::Observe(const TerrainAuthoringDocument &document) {
        const auto fence = document.Fence();
        if (document.IsClosed() || fence.session != session_ || fence.dataset != dataset_) {
            Clear();
            return;
        }
        std::erase_if(ids_, [&document](auto id) {
            return !document.Placements().contains(id);
        });
    }
}  // namespace Horo::Editor
