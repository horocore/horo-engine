#include "Horo/Application/PhysicsTriggerVolumeHost.h"

#include <type_traits>

int main() {
    static_assert(
        std::is_same_v<decltype(Horo::Application::CaptureProjectTriggerVolumeBinding(Horo::Physics::PhysicsWorldSimulationBinding{},
                                                                                      Horo::Physics::CollisionProfileId{},
                                                                                      Horo::Physics::PhysicsMaterialAsset{},
                                                                                      Horo::Assets::AssetCookArtifact{})),
                       Horo::Result<Horo::SceneSource::TriggerVolumeConversionContext>>);
    static_assert(Horo::Physics::PhysicsMaterialPayloadSize == 56);
    const auto malformedMaterial = Horo::Physics::DecodePhysicsMaterialAsset({});
    if (!malformedMaterial.HasError())
        return 2;
    const auto absent = Horo::Application::CaptureProjectTriggerVolumeBinding({}, {}, {}, {});
    return absent.HasError() ? 0 : 1;
}
