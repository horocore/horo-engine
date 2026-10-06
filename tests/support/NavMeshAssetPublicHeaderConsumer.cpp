#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/NavMeshAssetLoading.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Navigation::LoadedNavMeshAsset>);
static_assert(std::is_copy_constructible_v<Horo::Assets::AssetPayloadLease>);

int main() {
    const auto cache = Horo::Assets::AssetPayloadCache::Create(2, 1024);
    if (cache.HasError())
        return 1;
    Horo::Assets::AssetRegistry registry;
    const auto snapshot = registry.Snapshot();
    Horo::Assets::MemoryAssetProvider provider;
    const Horo::Navigation::NavMeshAssetSource source{snapshot, provider};
    const auto id = Horo::Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc");
    const auto target = Horo::AssetCookTargetId::Parse("macos-arm64");
    if (id.HasError() || target.HasError())
        return 1;
    Horo::CancellationSource cancellation;
    const auto missing = Horo::Navigation::LoadNavMeshAsset(source, id.Value(), target.Value(), *cache.Value(), cancellation.Token());
    return missing.HasError() &&
                   missing.ErrorValue().domain.Value() == Horo::Navigation::NavigationErrors::NoNavigationData.domain.Value() &&
                   missing.ErrorValue().code.Value() == Horo::Navigation::NavigationErrors::NoNavigationData.code.Value()
               ? 0
               : 1;
}
