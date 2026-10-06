#include "Horo/Prefab/PrefabTemplateCook.h"

#include <type_traits>

static_assert(
    std::is_same_v<decltype(&Horo::Prefab::CookPrefabTemplate),
                   Horo::Result<Horo::Prefab::CookedPrefab> (*)(
                       const Horo::Prefab::PrefabSourceResolverSnapshot &, const Horo::Assets::AssetRegistrySnapshot &,
                       Horo::Assets::AssetId, std::span<const Horo::Prefab::PrefabTemplateCookResource>, const Horo::AssetCookTargetId &,
                       const Horo::Prefab::PrefabLimitProfile &, const Horo::CancellationToken &, const Horo::Assets::AssetCookLimits &)>);

int main() {
    return 0;
}
