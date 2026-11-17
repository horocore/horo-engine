#include "HeadlessNetworkServices.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <catch2/catch_test_macros.hpp>

using namespace Horo;

TEST_CASE("Explicit headless prefab policy rejects before backend admission and preserves declared defaults", "[prefab][settings][host]") {
    Assets::AssetRegistry registry;
    Assets::MemoryAssetProvider bytes;
    JobSystem jobs{JobSystemConfig{1, 8}};
    Assets::AssetLoadService loads{jobs, bytes};
    CancellationSource cancellation;
    Application::Internal::HeadlessSceneLoadBoundary boundary{cancellation.Token(),
                                                              std::chrono::steady_clock::now() + std::chrono::seconds{2}};
    Runtime::SceneDefinitionBuilder builder{{7}, {1}};
    builder.Add({.object = {1}});
    const auto built = std::move(builder).Build();
    REQUIRE(built.HasValue());
    auto definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(built.Value());
    const Physics::PhysicsSceneActivationSettings settings{Physics::PhysicsWorldSettings::Capture({}).Value(),
                                                           Character::CharacterWorldSettings::Capture({}).Value(),
                                                           {}};
    for (const auto field : {0, 1, 2}) {
        Application::Internal::HeadlessSceneActivationInput input{static_cast<Physics::PhysicsRuntimeMode>(255), settings, registry, loads,
                                                                  boundary};
        if (field == 0)
            input.prefabPolicy.maximumRuntimeSpawnDepth = 0;
        if (field == 1)
            input.prefabPolicy.maximumInstanceBindings = 0;
        if (field == 2)
            input.prefabPolicy.maximumObjectCount = 0;
        const auto rejected = Application::Internal::ComposeHeadlessSceneNetworkServices(definition, input);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Prefab::PrefabErrors::LimitProfileInvalid.code.Value());
    }
    Application::Internal::HeadlessSceneActivationInput valid{Physics::PhysicsRuntimeMode::Null, settings, registry, loads, boundary};
    valid.prefabPolicy.maximumRuntimeSpawnDepth = 1;
    CHECK(Application::Internal::ComposeHeadlessSceneNetworkServices(definition, valid).HasValue());
}
