#include "OwnedTestDirectory.h"
#include "navigation/NavigationNativeBakeFixture.h"

namespace Horo::Navigation {
    using namespace AssetTestSupport;

    TEST_CASE("Native multi-Scene content requires exact scope and pins only the requested Scene",
              "[unit][navigation][navmesh_asset][scope]") {
        const std::array scopes{NavigationBakeScopeId::ForScene(Asset().Bytes()).Value(),
                                NavigationBakeScopeId::ForScene(Asset(true).Bytes()).Value()};
        Horo::Tests::OwnedTestDirectory directory{"scoped native navigation"};
        const auto encoded = BakeCanonicalContent(directory.Path(), scopes);
        auto cache = Assets::AssetPayloadCache::Create(16, 64 * 1024).Value();
        const Assets::AssetDependency metadata{Asset(), Type()};
        const auto missing = LoadNavMeshAsset(metadata, {1}, encoded, Target(), *cache);
        TestSupport::RequireError(missing, NavigationErrors::NoNavigationData);
        CHECK(cache->Snapshot().residentEntries == 0);
        const auto first = LoadNavMeshAsset(metadata, {1}, encoded, Target(), *cache, {.scope = scopes[0]});
        const auto second = LoadNavMeshAsset(metadata, {1}, encoded, Target(), *cache, {.scope = scopes[1]});
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().partitions.size() == 1);
        REQUIRE(second.Value().partitions.size() == 1);
        CHECK(first.Value().partitions.front().scope == scopes[0]);
        CHECK(second.Value().partitions.front().scope == scopes[1]);
        CHECK(first.Value().tileBytes.size() == 4);
        CHECK(second.Value().tileBytes.size() == 4);
        CHECK(first.Value().cookedContentDigest == second.Value().cookedContentDigest);
        CHECK_FALSE(first.Value().tileBytes.front().SharesAllocationWith(second.Value().tileBytes.front()));
        TestSupport::RequireError(LoadNavMeshAsset(metadata, {1}, Cooked(), Target(), *cache, {.scope = scopes[0]}),
                                  NavigationErrors::NoNavigationData);
    }

}  // namespace Horo::Navigation
