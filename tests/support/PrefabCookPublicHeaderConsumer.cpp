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
    const auto intent = Horo::Packages::ValidatedPackageRequest::Parse(R"({"sources":{},"dependencies":{}})");
    if (intent.HasError())
        return 1;
    const Horo::SceneSource::SceneSourceDocument source;
    const auto converted = Horo::SceneSource::ConvertSceneSourceToRuntime({source.objects, source.prefabInstances}, {1}, {1});
    if (converted.HasError())
        return 1;
    const auto encoded = Horo::SceneCook::EncodeCookedSceneDefinition(converted.Value());
    if (encoded.HasError())
        return 1;
    const auto decoded = Horo::SceneCook::DecodeCookedSceneDefinition(encoded.Value(), {1}, {1});
    return decoded.HasValue() && decoded.Value().Entities().empty() ? 0 : 1;
}
