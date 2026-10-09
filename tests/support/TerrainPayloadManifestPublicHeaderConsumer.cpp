#include "Horo/Terrain/TerrainPayloadManifest.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Terrain::TerrainPayloadManifest>);
static_assert(!std::is_copy_assignable_v<Horo::Terrain::TerrainPayloadManifest>);
static_assert(std::is_move_constructible_v<Horo::Terrain::TerrainPayloadManifest>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Terrain::TerrainPayloadManifest &>().Tiles()),
                             std::span<const Horo::Terrain::TerrainTilePayloadEntry>>);

int main() {
    const auto invalid = Horo::Terrain::GenerateTerrainPayloadManifest({});
    return invalid.HasError() ? 0 : 1;
}
