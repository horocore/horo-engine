#pragma once

#include "Horo/Destruction/OfflineVoronoi.h"

namespace Horo::Destruction::Detail {
    [[nodiscard]] Sha256Digest VoronoiFingerprint(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                                  const std::vector<std::array<double, 3>> &sites);
    [[nodiscard]] std::uint64_t VoronoiOutputChecksum(const OfflineVoronoiCandidate &candidate);
}  // namespace Horo::Destruction::Detail
