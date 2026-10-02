#include "Horo/Terrain/FoliagePlacementCook.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Terrain::CookedFoliagePlacement>);
static_assert(std::is_move_constructible_v<Horo::Terrain::CookedFoliagePlacement>);

int main() {
    Horo::Terrain::FoliagePlacementCookOwner owner;
    return owner.Current() == nullptr && !owner.IsClosed() ? 0 : 1;
}
