#include "Horo/Application/TerrainAuthoringAssets.h"
#include "Horo/Application/TerrainAuthoringCook.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Application::TerrainAuthoringCookHost>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Application::TerrainAuthoringCookCompletion>);

int main() {
    return 0;
}
