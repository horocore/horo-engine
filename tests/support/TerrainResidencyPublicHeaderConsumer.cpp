#include "Horo/TerrainStreaming/TerrainResidencyCache.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Terrain::TerrainResidencyCache>);
static_assert(!std::is_move_constructible_v<Horo::Terrain::TerrainResidencyCache>);

template <class Cache>
concept CanBypassFactoryAdmission =
    requires(Horo::Terrain::TerrainResidencyOwnerId owner, Horo::WorldStreaming::StreamingFeatureBudgetReservations &authority,
             Horo::Terrain::TerrainResidencyLimits limits) { Cache{owner, authority, limits, {}}; };
static_assert(!CanBypassFactoryAdmission<Horo::Terrain::TerrainResidencyCache>);

int main() {
    return Horo::Terrain::TerrainResidencyKey{}.IsValid() ? 1 : 0;
}
