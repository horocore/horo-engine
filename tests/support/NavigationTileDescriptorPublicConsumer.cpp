#include "Horo/Navigation/NavigationTileDescriptor.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::Navigation::ProjectNavigationCookedTileDescriptor(
                                 std::declval<const Horo::Navigation::NavigationCookedTile &>())),
                             Horo::Result<Horo::Navigation::NavigationCookedTileDescriptor>>);

int main() {
    return 0;
}
