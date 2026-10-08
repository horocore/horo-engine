#include "Horo/Terrain/TerrainSourceArtifacts.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Terrain::CookedTerrainSourceArtifacts>);
static_assert(!std::is_move_assignable_v<Horo::Terrain::CookedTerrainSourceArtifacts>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Terrain::CookedTerrainSourceArtifacts>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Terrain::CookedTerrainSourceArtifacts &>().Artifacts()),
                             std::span<const Horo::Terrain::TerrainSourceArtifact>>);

int main() {
    const auto absent = Horo::Terrain::CookTerrainSourceArtifacts({}, {}, {}, {});
    const auto corrupt = Horo::Terrain::VerifyTerrainSourceArtifactPayload({}, {});
    return Horo::Terrain::CurrentTerrainSourceArtifactSchema == 1 && absent.HasError() && corrupt.HasError() ? 0 : 1;
}
