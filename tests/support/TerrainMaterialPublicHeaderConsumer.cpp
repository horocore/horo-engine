#include "Horo/Terrain/TerrainMaterial.h"
#include "Horo/TerrainRender/TerrainMaterialBinding.h"

#include <type_traits>

static_assert(!std::is_same_v<Horo::Terrain::TerrainLayerId, Horo::Terrain::TerrainMaterialAssetId>);
static_assert(!std::is_copy_constructible_v<Horo::TerrainRender::TerrainMaterialBindingOwner>);

int main() {
    Horo::TerrainRender::TerrainMaterialBindingOwner owner;
    const auto snapshot = owner.Snapshot();
    return snapshot.HasValue() && snapshot.Value() == nullptr && owner.Shutdown().HasValue() ? 0 : 1;
}
