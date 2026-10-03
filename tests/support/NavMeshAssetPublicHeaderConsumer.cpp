#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/NavMeshAssetLoading.h"
#include "Horo/Navigation/NavMeshCodec.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Navigation::LoadedNavMeshAsset>);
static_assert(std::is_copy_constructible_v<Horo::Assets::AssetPayloadLease>);

int main() {
    const auto cache = Horo::Assets::AssetPayloadCache::Create(2, 1024);
    return cache.HasValue() && Horo::Assets::NavMeshAssetTypeName == "core.navmesh" ? 0 : 1;
}
