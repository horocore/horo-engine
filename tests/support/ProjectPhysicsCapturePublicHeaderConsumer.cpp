#include "Horo/Application/ProjectPhysicsCapture.h"
#include "Horo/Physics/PhysicsErrors.h"

#if __has_include("Horo/Application/PrefabSceneCookHost.h") || __has_include("Horo/Editor/EditorServiceRegistry.h")
#error "Runtime Project/Physics capture must not expose Prefab cook or Editor service ownership."
#endif

int main() {
    const auto id = Horo::Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc");
    if (id.HasError())
        return 1;
    const auto material = Horo::Physics::PhysicsMaterialAsset::Create(
        {.id = Horo::Physics::PhysicsMaterialAssetId::FromAssetId(id.Value()), .sourceRevision = 1});
    if (material.HasError())
        return 1;
    // This consumer performs actual typed admission, without selecting a solver or
    // fabricating project authority to make an intentionally incomplete capture pass.
    const auto captured = Horo::Application::CaptureProjectTriggerVolumeBinding({}, {}, material.Value(), {});
    return captured.HasError() && captured.ErrorValue().code.Value() == Horo::Physics::PhysicsErrors::DescriptorInvalid.code.Value() ? 0
                                                                                                                                     : 1;
}
