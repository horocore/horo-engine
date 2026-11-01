#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::Terrain::TerrainAuthoringLayerBinding::order),
                             std::array<Horo::Terrain::TerrainLayerId, Horo::Terrain::TerrainDescriptorHardLimits::LayersPerTile>>);

int main() {
    return Horo::Terrain::IsTerrainAuthoringSource({}) ? 1 : 0;
}
