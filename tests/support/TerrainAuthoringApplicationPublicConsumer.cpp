#include "Horo/Application/TerrainAuthoringAssets.h"
#include "Horo/Application/TerrainAuthoringCook.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Application::TerrainAuthoringCookHost>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Application::TerrainAuthoringCookCompletion>);

static_assert(std::is_same_v<Horo::Editor::TerrainAuthoredPlacement, Horo::Terrain::TerrainAuthoredPlacement>);

int main() {
    return 0;
}
