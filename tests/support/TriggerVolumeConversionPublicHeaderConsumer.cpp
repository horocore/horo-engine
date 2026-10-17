#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

#include <array>
#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Runtime::TriggerVolumeProjectBinding>);
static_assert(std::is_copy_constructible_v<Horo::SceneSource::TriggerVolumeConversionContext>);

int main() {
    using namespace Horo;
    std::array objects{
        SceneSource::SceneObjectSnapshot{.id = {1}, .components = {.triggerVolume = Runtime::TriggerVolumeComponent{.enabled = false}}}};
    const auto disabled = SceneSource::ConvertSceneSourceToRuntime({objects, {}}, {1}, {1});
    if (disabled.HasError() || !disabled.Value().TriggerVolumeEvidence().empty())
        return 1;
    Assets::AssetRegistry registry;
    const auto limits = Prefab::PrefabLimitProfile::Create({});
    if (limits.HasError())
        return 2;
    const auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {}, limits.Value());
    if (resolver.HasError())
        return 3;
    const SceneSource::ScenePrefabResolutionContext resolution{resolver.Value(), limits.Value()};
    const auto retained = SceneSource::ConvertScenePrefabProjectionToRuntime({objects, {}}, {1}, {1}, {}, resolution);
    if (retained.HasError() || !retained.Value().TriggerVolumeEvidence().empty())
        return 4;
    objects.front().components.triggerVolume->enabled = true;
    const auto missing = SceneSource::ConvertSceneSourceToRuntime({objects, {}}, {1}, {2});
    return missing.HasError() && missing.ErrorValue().code.Value() == "scene_conversion.trigger_volume_project_binding_missing" ? 0 : 5;
}
