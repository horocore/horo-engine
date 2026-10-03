#include "Horo/Terrain/FoliageClusterCook.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Terrain::CookedFoliageClusterSet>);
static_assert(!std::is_same_v<Horo::Terrain::FoliageInstanceId, Horo::Terrain::FoliageClusterId>);

int main() {
    Horo::Terrain::FoliageClusterCookOwner owner;
    owner.Close();
    return owner.IsClosed() && owner.Current() == nullptr ? 0 : 1;
}
