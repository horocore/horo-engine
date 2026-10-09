#include "Horo/Terrain/TerrainProducerSnapshot.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Terrain::TerrainProducerSnapshot>);
static_assert(!std::is_default_constructible_v<Horo::Terrain::TerrainProducerSnapshot>);

int main() {
    Horo::Terrain::TerrainProducerSnapshotRequest request;
    const auto rejected = Horo::Terrain::CaptureTerrainProducerSnapshot(request);
    Horo::Terrain::TerrainProducerSnapshotOwner owner;
    owner.Shutdown();
    return rejected.HasError() && owner.Snapshot().HasError() ? 0 : 1;
}
