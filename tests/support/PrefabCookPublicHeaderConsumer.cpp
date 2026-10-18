#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Assets::AssetCookInputSnapshot>);
static_assert(!std::is_default_constructible_v<Horo::Assets::AssetCookInputSnapshot>);
static_assert(!std::is_default_constructible_v<Horo::Application::PrefabSceneCookHost>);
static_assert(!std::is_default_constructible_v<Horo::Application::PrefabCookSchemaContext>);
static_assert(std::is_same_v<decltype(Horo::Application::PrefabSceneCookRequest{}.runtimePrefabRoots), std::vector<Horo::Assets::AssetId>>);

int main() {
    if (const auto intent = Horo::Packages::ValidatedPackageRequest::Parse(R"({"sources":{},"dependencies":{}})"); intent.HasError())
        return 1;
    const auto asset = Horo::Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc");
    if (asset.HasError())
        return 1;
    const auto scene = Horo::SceneCook::SceneDefinitionIdForAsset(asset.Value());
    if (scene != Horo::Runtime::SceneDefinitionId{2234773191861595960ULL})
        return 1;
    const Horo::SceneSource::SceneSourceDocument source;
    const auto converted = Horo::SceneSource::ConvertSceneSourceToRuntime({source.objects, source.prefabInstances}, scene, {1});
    if (converted.HasError())
        return 1;
    const auto encoded = Horo::SceneCook::EncodeCookedSceneDefinition(converted.Value());
    if (encoded.HasError())
        return 1;
    const auto decoded = Horo::SceneCook::DecodeCookedSceneDefinition(encoded.Value(), scene, {1});
    const auto embedded = Horo::SceneCook::DecodeCookedSceneWithEmbeddedRevision(encoded.Value(), scene);
    return decoded.HasValue() && embedded.HasValue() && embedded.Value().Revision() == Horo::Runtime::SceneDefinitionRevision{1} &&
                   decoded.Value().Entities().empty()
               ? 0
               : 1;
}
