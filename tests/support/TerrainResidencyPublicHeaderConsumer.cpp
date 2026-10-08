#include "Horo/TerrainStreaming/TerrainResidencyCache.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Terrain::TerrainResidencyCache>);
static_assert(!std::is_move_constructible_v<Horo::Terrain::TerrainResidencyCache>);

int main() {
    return Horo::Terrain::TerrainResidencyKey{}.IsValid() ? 1 : 0;
}
