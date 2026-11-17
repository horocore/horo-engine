#pragma once
/** @file FoliageSelectionModel.h @brief Stable-ID selection observes placements without owning or mutating them. */
#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <span>

namespace Horo::Editor {
    /** @brief Owner-lane transient selection, scoped to one actual document session and dataset. */
    class FoliageSelectionModel final {
    public:
        /** @brief Atomically select exact existing authored IDs; selection never submits a terrain edit.
         * @param document Actual open authoring owner. @param fence Exact currently viewed publication.
         * @param ids Bounded stable identities; duplicates collapse deterministically.
         * @return Success or typed stale/closed/missing/capacity failure, preserving the previous selection. */
        [[nodiscard]] Result<void> Replace(const TerrainAuthoringDocument &document, const TerrainEditFence &fence,
                                           std::span<const Terrain::FoliageInstanceId> ids);
        /** @brief Retain surviving IDs after ordinary edits; replacement/close clears every old-session borrow.
         * @param document Current sole placement owner. No source or history mutation occurs. */
        void Observe(const TerrainAuthoringDocument &document);

        /** @brief Borrow current selected IDs until the next selection/document observation. @return Sorted stable identities. */
        [[nodiscard]] std::span<const Terrain::FoliageInstanceId> Ids() const noexcept {
            return ids_;
        }

        /** @brief Discard transient selection at tool/workspace shutdown. */
        void Clear() noexcept {
            ids_.clear();
            session_ = {};
            dataset_ = {};
        }

    private:
        TerrainDocumentSessionId session_;
        Terrain::TerrainDatasetId dataset_;
        std::vector<Terrain::FoliageInstanceId> ids_;
    };
}  // namespace Horo::Editor
